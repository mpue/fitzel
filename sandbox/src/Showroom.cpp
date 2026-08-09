#include "Showroom.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <glm/gtc/constants.hpp>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/scene/Camera.hpp>

#include "SceneTypes.hpp"

namespace showroom {
namespace {

// --- Small maths ------------------------------------------------------------
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
// Normalise a raw craft figure into the 0..1 a bar draws, so two craft can be
// compared at a glance instead of by reading numbers.
float norm(float v, float lo, float hi) { return clamp01((v - lo) / (hi - lo)); }
float easeOut(float t) { const float u = 1.0f - clamp01(t); return 1.0f - u * u * u; }
float easeInOut(float t) {
    const float u = clamp01(t);
    return u < 0.5f ? 4.0f * u * u * u : 1.0f - std::pow(-2.0f * u + 2.0f, 3.0f) * 0.5f;
}
// Frame-rate independent exponential approach.
float approach(float cur, float target, float rate, float dt) {
    return cur + (target - cur) * (1.0f - std::exp(-rate * dt));
}

// A stable seed from a name, so a circuit's generated glyph is the same every
// time it is shown (and different from its neighbours').
unsigned hashName(const std::string& s) {
    unsigned h = 2166136261u;
    for (const char c : s) { h ^= static_cast<unsigned char>(c); h *= 16777619u; }
    return h;
}
float seeded(unsigned h, int i) {   // 0..1, deterministic per (seed, index)
    unsigned x = h + static_cast<unsigned>(i) * 2654435761u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
    return static_cast<float>(x & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
}

// --- Colour -----------------------------------------------------------------
constexpr ImU32 kInk     = IM_COL32(  8,  11,  17, 214);
constexpr ImU32 kInkSoft = IM_COL32( 12,  16,  24, 150);
constexpr ImU32 kEdge    = IM_COL32(255, 255, 255,  30);
constexpr ImU32 kText    = IM_COL32(238, 242, 249, 255);
constexpr ImU32 kDim     = IM_COL32(146, 158, 178, 255);

ImU32 fade(ImU32 col, float a) {
    const float f = clamp01(a);
    return (col & 0x00FFFFFFu) |
           (static_cast<ImU32>(((col >> 24) & 0xFF) * f) << 24);
}
ImU32 rgba(const glm::vec3& c, float a) {
    return IM_COL32(static_cast<int>(clamp01(c.r) * 255.0f),
                    static_cast<int>(clamp01(c.g) * 255.0f),
                    static_cast<int>(clamp01(c.b) * 255.0f),
                    static_cast<int>(clamp01(a) * 255.0f));
}

// --- Draw context -----------------------------------------------------------
// Everything is expressed in one scale unit, so the screen keeps its proportions
// from a docked editor viewport up to a 4K presentation window.
struct Ctx {
    ImDrawList* dl = nullptr;
    ImFont*     font = nullptr;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx = 0, cy = 0;
    float S  = 1.0f;
    float fTiny = 0, fSmall = 0, fBody = 0, fBig = 0, fHuge = 0;
    float t = 0.0f;                 // seconds, for the idle animation
    glm::vec3 acc{0.4f, 0.9f, 1.0f}; // the craft's accent, the whole UI's colour
    bool  mouse = true;             // the mouse is ours this frame

    float textW(float size, const char* s) const {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    }
    void text(float x, float y, float size, ImU32 col, const char* s) const {
        const float o = std::max(1.0f, size * 0.06f);
        dl->AddText(font, size, ImVec2(x + o, y + o),
                    fade(IM_COL32(0, 0, 0, 210), ((col >> 24) & 0xFF) / 255.0f), s);
        dl->AddText(font, size, ImVec2(x, y), col, s);
    }
    void textC(float xm, float y, float size, ImU32 col, const char* s) const {
        text(xm - textW(size, s) * 0.5f, y, size, col, s);
    }
    void textR(float xr, float y, float size, ImU32 col, const char* s) const {
        text(xr - textW(size, s), y, size, col, s);
    }
    ImU32 accent(float a) const { return rgba(acc, a); }
    // Letter-spaced small caps: the showroom's one typographic flourish, used for
    // the labels that frame everything else.
    void spaced(float x, float y, float size, ImU32 col, const char* s,
                float extra = 0.28f) const {
        float cx2 = x;
        for (const char* p = s; *p; ++p) {
            const char b[2] = {*p, '\0'};
            text(cx2, y, size, col, b);
            cx2 += textW(size, b) + size * extra;
        }
    }
    float spacedW(float size, const char* s, float extra = 0.28f) const {
        float w = 0.0f;
        for (const char* p = s; *p; ++p) {
            const char b[2] = {*p, '\0'};
            w += textW(size, b) + size * extra;
        }
        return w;
    }
};

// A translucent panel with a hairline edge and an accent bar down its left side.
void panel(const Ctx& g, float ax, float ay, float bx, float by, float alpha,
           bool accentBar = true) {
    const float r = 6.0f * g.S;
    g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by), fade(kInk, alpha), r);
    g.dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by), fade(kEdge, alpha), r, 0, 1.0f);
    if (accentBar)
        g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + 3.0f * g.S, by),
                            g.accent(0.85f * alpha), r, ImDrawFlags_RoundCornersLeft);
}

// One telemetry bar: a track of ticks with a filled run and the figure at the end.
void statBar(const Ctx& g, float x, float y, float w, float h,
             const char* label, float v01, const char* value, float alpha) {
    g.spaced(x, y, g.fTiny, fade(kDim, alpha), label);
    const float by0 = y + g.fTiny * 1.65f;
    g.dl->AddRectFilled(ImVec2(x, by0), ImVec2(x + w, by0 + h),
                        fade(IM_COL32(255, 255, 255, 22), alpha), h * 0.5f);
    const float fw = w * clamp01(v01);
    if (fw > 1.0f) {
        g.dl->AddRectFilledMultiColor(
            ImVec2(x, by0), ImVec2(x + fw, by0 + h),
            g.accent(0.55f * alpha), g.accent(1.0f * alpha),
            g.accent(1.0f * alpha), g.accent(0.55f * alpha));
        // The head of the run glows, so the eye lands on where the value ends.
        g.dl->AddRectFilled(ImVec2(x + fw - 2.0f * g.S, by0 - 2.0f * g.S),
                            ImVec2(x + fw + 2.0f * g.S, by0 + h + 2.0f * g.S),
                            g.accent(0.9f * alpha), 2.0f * g.S);
    }
    // Ticks every tenth: a scale to read the run against.
    for (int i = 1; i < 10; ++i) {
        const float tx = x + w * (i / 10.0f);
        g.dl->AddLine(ImVec2(tx, by0), ImVec2(tx, by0 + h),
                      fade(IM_COL32(6, 9, 14, 190), alpha), 1.0f);
    }
    if (value) g.textR(x + w, y, g.fTiny, fade(kText, alpha), value);
}

// A large button. Hovering selects it, so the mouse never has to hit anything
// small or be held steady while clicking -- the same rule the race's end-of-race
// question follows.
bool bigButton(const Ctx& g, float ax, float ay, float bx, float by,
               const char* label, bool selected, float alpha, int* hovered,
               float pulse) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool over = g.mouse && m.x >= ax && m.x <= bx && m.y >= ay && m.y <= by;
    if (over && hovered) *hovered = 1;
    const bool lit = selected || over;
    const float r = 8.0f * g.S;
    if (lit) {
        // A soft halo, breathing, so the armed target is obvious across the room.
        for (int i = 3; i >= 1; --i) {
            const float o = i * 3.0f * g.S * (1.0f + pulse * 0.5f);
            g.dl->AddRect(ImVec2(ax - o, ay - o), ImVec2(bx + o, by + o),
                          g.accent(0.10f * alpha / i), r + o, 0, 2.0f * g.S);
        }
    }
    g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by),
                        lit ? g.accent(0.20f * alpha) : fade(kInk, alpha), r);
    g.dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by),
                  lit ? g.accent(0.95f * alpha) : fade(kEdge, alpha), r, 0,
                  lit ? 2.4f * g.S : 1.0f);
    const float fs = g.fBig * 0.72f;
    const float lw = g.spacedW(fs, label, 0.16f);
    g.spaced((ax + bx) * 0.5f - lw * 0.5f, (ay + by) * 0.5f - fs * 0.62f, fs,
             lit ? g.accent(alpha) : fade(kText, alpha), label, 0.16f);
    return over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// A chevron: the craft list's left/right step. Deliberately huge -- this is the
// control that gets used most, and it must not need a steady hand.
bool chevron(const Ctx& g, float cx2, float cy2, float size, int dir,
             float alpha, bool armed) {
    const float hit = size * 1.25f;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool over = g.mouse && std::abs(m.x - cx2) <= hit && std::abs(m.y - cy2) <= hit;
    const float a = alpha * (over ? 1.0f : (armed ? 0.75f : 0.35f));
    const float w = size * 0.42f, h = size * 0.62f;
    const float th = std::max(2.0f, size * 0.13f);
    const ImVec2 p0(cx2 + dir * -w * 0.4f, cy2 - h);
    const ImVec2 p1(cx2 + dir *  w * 0.6f, cy2);
    const ImVec2 p2(cx2 + dir * -w * 0.4f, cy2 + h);
    g.dl->AddLine(p0, p1, g.accent(a), th);
    g.dl->AddLine(p1, p2, g.accent(a), th);
    if (over)
        g.dl->AddCircle(ImVec2(cx2, cy2), size * 1.1f, g.accent(a * 0.4f), 32, 1.5f);
    return over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// A generated circuit glyph: a closed loop built from a few harmonics off the
// name's hash. It stands in for a preview image a project hasn't drawn yet, and
// it is stable per circuit -- so the cards look authored rather than blank.
void circuitGlyph(const Ctx& g, float ax, float ay, float bx, float by,
                  unsigned seed, ImU32 col, float thick, float phase) {
    const float rx = (bx - ax) * 0.36f, ry = (by - ay) * 0.36f;
    const float mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f;
    const float a1 = 0.10f + seeded(seed, 1) * 0.22f;
    const float a2 = 0.06f + seeded(seed, 2) * 0.20f;
    const float a3 = 0.04f + seeded(seed, 3) * 0.12f;
    const float p1 = seeded(seed, 4) * 6.28f, p2 = seeded(seed, 5) * 6.28f;
    const float p3 = seeded(seed, 6) * 6.28f;
    constexpr int kN = 84;
    ImVec2 pts[kN];
    for (int i = 0; i < kN; ++i) {
        const float th = (i / static_cast<float>(kN)) * 6.28318f;
        const float r = 1.0f + a1 * std::sin(2.0f * th + p1) +
                               a2 * std::sin(3.0f * th + p2) +
                               a3 * std::sin(5.0f * th + p3);
        pts[i] = ImVec2(mx + std::cos(th + phase) * rx * r,
                        my + std::sin(th + phase) * ry * r);
    }
    g.dl->AddPolyline(pts, kN, col, thick, ImDrawFlags_Closed);
}

// Difficulty as pips: five slots, the filled ones in the accent.
void pips(const Ctx& g, float x, float y, float size, float value, float alpha) {
    for (int i = 0; i < 5; ++i) {
        const float px = x + i * size * 1.7f;
        const bool on = value >= i + 0.5f;
        g.dl->AddRectFilled(ImVec2(px, y), ImVec2(px + size, y + size * 1.5f),
                            on ? g.accent(0.95f * alpha)
                               : fade(IM_COL32(255, 255, 255, 40), alpha),
                            size * 0.25f);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

bool Showroom::isShowroomScene(const std::vector<Entity>& entities) {
    for (const Entity& e : entities)
        if (e.active && e.components.get<ShowroomComponent>()) return true;
    return false;
}

void Showroom::begin(const std::vector<Entity>& entities,
                     const std::vector<std::string>& fallbackScenes,
                     const fitzel::Camera& camera) {
    m_craft.clear();
    m_tracks.clear();
    m_cues.clear();
    m_podiumId = -1;

    const ShowroomComponent* stage = nullptr;
    for (const Entity& e : entities)
        if (const auto* sc = e.components.get<ShowroomComponent>(); sc && e.active) {
            stage = sc; m_podiumId = e.id; m_podium = e.center; break;
        }
    if (!stage) { m_active = false; return; }

    m_title      = stage->title;
    m_subtitle   = stage->subtitle;
    m_ringRadius = stage->ringRadius;
    m_riseHeight = stage->riseHeight;
    m_spinSpeed  = stage->spinSpeed;
    m_bob        = stage->bobAmount;
    m_camDist    = std::max(1.0f, stage->camDistance);
    m_camHeight  = stage->camHeight;
    m_camPitch   = stage->camPitch;
    m_camOrbit   = stage->camOrbit;
    m_camFov     = stage->camFov;
    m_accent     = stage->accent;
    m_sndMove    = stage->soundMove;
    m_sndSelect  = stage->soundSelect;
    m_sndStart   = stage->soundStart;
    m_sndGain    = stage->soundGain;

    // Craft: every glider in the scene. A Craft Entry beside it fills in the
    // presentation; without one the entity's own name and numbers carry the card.
    for (const Entity& e : entities) {
        const auto* gc = e.components.get<GliderComponent>();
        if (!gc) continue;
        Craft c;
        c.id      = e.id;
        c.title   = e.name;
        c.accent  = m_accent;
        c.isRoot  = e.parent < 0;
        c.homeLocal  = e.localCenter;
        c.homeRot    = e.localRotation;
        c.homeWorld  = e.center;
        c.homeActive = e.active;
        if (const auto* ce = e.components.get<CraftEntryComponent>()) {
            if (!ce->title.empty()) c.title = ce->title;
            c.team   = ce->team;
            c.blurb  = ce->blurb;
            c.accent = ce->accent;
            c.order  = ce->order;
        }
        if (c.title.empty()) c.title = "CRAFT";
        // The bars, straight off the flight model -- what the card promises is
        // what the craft does.
        c.topSpeedMps = gc->maxSpeed;
        c.speed   = norm(gc->maxSpeed,  20.0f, 160.0f);
        c.accel   = norm(gc->thrust,     6.0f,  90.0f);
        c.grip    = norm(gc->grip,       0.0f,   8.0f);
        c.agility = norm(gc->turnRate,  40.0f, 220.0f);
        m_craft.push_back(std::move(c));
    }
    std::stable_sort(m_craft.begin(), m_craft.end(),
                     [](const Craft& a, const Craft& b) { return a.order < b.order; });

    // Circuits: the scene's Track Entries, or -- when it authors none -- every
    // other scene in the project, so the picker is never empty.
    for (const Entity& e : entities) {
        const auto* te = e.components.get<TrackEntryComponent>();
        if (!te || !e.active || te->scene.empty()) continue;
        Track t;
        t.scene      = te->scene;
        t.title      = te->title.empty() ? te->scene : te->title;
        t.blurb      = te->blurb;
        t.image      = te->image;
        t.laps       = static_cast<int>(std::lround(te->laps));
        t.lengthKm   = te->lengthKm;
        t.difficulty = te->difficulty;
        t.order      = te->order;
        m_tracks.push_back(std::move(t));
    }
    std::stable_sort(m_tracks.begin(), m_tracks.end(),
                     [](const Track& a, const Track& b) { return a.order < b.order; });
    if (m_tracks.empty())
        for (const std::string& s : fallbackScenes) {
            Track t; t.scene = s; t.title = s; m_tracks.push_back(std::move(t));
        }

    // The camera is borrowed from here until end() gives it back.
    m_camHome      = camera.position();
    m_camHomeYaw   = camera.yaw();
    m_camHomePitch = camera.pitch();
    m_camHomeFov   = camera.fov();

    m_craftSel = m_trackSel = 0;
    m_prevCraft = -1;
    m_row      = Row::Craft;
    m_time = m_intro = m_swap = m_trackFx = m_launch = 0.0f;
    m_launching = false;
    m_wantBack  = false;
    m_orbit = 40.0f; m_orbitVel = 0.0f; m_spin = 0.0f;
    m_ringPos = 0.0f; m_stripPos = 0.0f;
    for (float& b : m_bar) b = 0.0f;
    m_active = true;
}

void Showroom::end(std::vector<Entity>& entities, fitzel::Camera& camera) {
    if (!m_active) return;
    // Give the camera back before anything else looks at it. The FOV especially:
    // a race never writes one, so whatever is left here is what it runs with.
    camera.setPosition(m_camHome);
    camera.setYaw(m_camHomeYaw);
    camera.setPitch(m_camHomePitch);
    camera.setFov(m_camHomeFov);
    // Put every craft back exactly as the scene had it. Without this the chosen
    // craft would be carried into the race in its showroom pose -- floating,
    // spun, and (for the ones that were off stage) deactivated.
    for (const Craft& c : m_craft)
        for (Entity& e : entities)
            if (e.id == c.id) {
                e.localCenter   = c.homeLocal;
                e.localRotation = c.homeRot;
                e.center        = c.homeWorld;
                e.rotation      = c.homeRot;
                e.active        = c.homeActive;
                break;
            }
    m_active = false;
    m_launching = false;
    m_texCache.clear();
}

glm::vec3 Showroom::accent() const {
    if (m_craftSel >= 0 && m_craftSel < static_cast<int>(m_craft.size()))
        return m_craft[m_craftSel].accent;
    return m_accent;
}

void Showroom::cue(const std::string& snd, float pitch) {
    if (snd.empty()) return;
    m_cues.push_back(Cue{snd, m_sndGain, pitch});
}

void Showroom::moveCraft(int dir) {
    if (m_craft.size() < 2) return;
    m_prevCraft = m_craftSel;
    const int n = static_cast<int>(m_craft.size());
    m_craftSel = (m_craftSel + dir % n + n) % n;
    m_swap = 1.0f;
    m_orbitVel += -dir * 130.0f;   // the camera swings with the change
    cue(m_sndSelect.empty() ? m_sndMove : m_sndSelect, 1.0f);
}

void Showroom::moveTrack(int dir) {
    if (m_tracks.size() < 2) return;
    const int n = static_cast<int>(m_tracks.size());
    m_trackSel = (m_trackSel + dir % n + n) % n;
    m_trackFx = 1.0f;
    cue(m_sndMove, 1.06f);
}

// ---------------------------------------------------------------------------
// Update: the stage, the camera, the clocks
// ---------------------------------------------------------------------------

void Showroom::update(std::vector<Entity>& entities, fitzel::Camera& camera,
                      float dt, const Input& in) {
    if (!m_active) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    m_time  += dt;
    m_intro  = std::min(1.0f, m_intro + dt * 1.6f);
    m_swap   = std::max(0.0f, m_swap - dt * 1.35f);
    m_trackFx = std::max(0.0f, m_trackFx - dt * 2.4f);

    // The launch owns the last second: input is done, the camera dives in and the
    // screen goes white. Handing over mid-animation would cut it off.
    if (m_launching) {
        m_launch = std::min(1.0f, m_launch + dt * 1.25f);
    } else {
        // --- Navigation -----------------------------------------------------
        if (in.up)   { m_row = static_cast<Row>(std::max(0, static_cast<int>(m_row) - 1));
                       cue(m_sndMove, 0.92f); }
        if (in.down) { m_row = static_cast<Row>(std::min(2, static_cast<int>(m_row) + 1));
                       cue(m_sndMove, 0.92f); }
        if (in.left || in.right) {
            const int d = in.right ? 1 : -1;
            if (m_row == Row::Craft) moveCraft(d);
            else                     moveTrack(d); // the START row still browses circuits
        }
        // Confirm walks forward through the rows and commits on the last one, so
        // the whole screen can be answered with one button held in one hand.
        if (in.confirm) {
            if (m_row != Row::Start) {
                m_row = static_cast<Row>(static_cast<int>(m_row) + 1);
                cue(m_sndMove, 1.0f);
            } else if (!m_craft.empty() && !m_tracks.empty()) {
                m_launching = true;
                m_launch    = 0.0f;
                cue(m_sndStart, 1.0f);
            }
        }
        if (in.back) m_wantBack = true;
    }

    // --- Stat bars ease toward the chosen craft ----------------------------
    if (m_craftSel >= 0 && m_craftSel < static_cast<int>(m_craft.size())) {
        const Craft& c = m_craft[m_craftSel];
        const float want[4] = {c.speed, c.accel, c.grip, c.agility};
        for (int i = 0; i < 4; ++i) {
            // Staggered rates: the bars arrive one after another rather than as a
            // block, which is what makes the swap read as a reveal.
            m_bar[i] = approach(m_bar[i], want[i], 6.0f + i * 1.4f, dt);
        }
    }

    // --- Camera: a slow orbit, swung by every change, dived on launch -------
    m_orbit    += (m_camOrbit + m_orbitVel) * dt;
    m_orbitVel  = approach(m_orbitVel, 0.0f, 3.2f, dt);
    const float launchE = easeInOut(m_launch);
    // Pulls back as a craft changes places, then settles; on launch it closes in.
    const float dist = m_camDist * (1.0f + 0.16f * m_swap - 0.45f * launchE);
    const float th   = glm::radians(m_orbit);
    const glm::vec3 eye = m_podium + glm::vec3(std::cos(th) * dist,
                                               m_camHeight + m_riseHeight * 0.5f,
                                               std::sin(th) * dist);
    camera.setPosition(eye);
    camera.setYaw(m_orbit + 180.0f);
    camera.setPitch(m_camPitch - 4.0f * launchE);
    camera.setFov(m_camFov + 5.0f * m_swap + 26.0f * launchE);

    // --- The craft on stage -------------------------------------------------
    m_spin += (m_spinSpeed + 260.0f * launchE) * dt;
    const float bob = std::sin(m_time * 1.35f) * m_bob;
    const int   sel = m_craftSel;
    const bool  ring = m_ringRadius > 0.01f && m_craft.size() > 1;
    if (ring) {
        // Carousel: the ring turns to bring the chosen craft to the front. The
        // eased index is unwrapped toward the target so it always takes the short
        // way round instead of unwinding the whole circle.
        const float n = static_cast<float>(m_craft.size());
        float d = static_cast<float>(sel) - m_ringPos;
        while (d >  n * 0.5f) d -= n;
        while (d < -n * 0.5f) d += n;
        m_ringPos = approach(m_ringPos, m_ringPos + d, 7.0f, dt);
        const float step = 6.28318f / n;
        for (std::size_t i = 0; i < m_craft.size(); ++i) {
            Entity* e = nullptr;
            for (Entity& x : entities) if (x.id == m_craft[i].id) { e = &x; break; }
            if (!e) continue;
            e->active = true;
            const bool isSel = static_cast<int>(i) == sel;
            const float a = (static_cast<float>(i) - m_ringPos) * step;
            const glm::vec3 p = m_podium + glm::vec3(std::sin(a) * m_ringRadius,
                                                     isSel ? m_riseHeight + bob : 0.0f,
                                                     std::cos(a) * m_ringRadius);
            const float yaw = isSel ? m_spin : glm::degrees(a) + 180.0f;
            if (m_craft[i].isRoot) { e->localCenter = p; e->center = p; }
            e->localRotation = glm::vec3(0.0f, yaw, 0.0f);
            e->rotation      = e->localRotation;
        }
    } else {
        // Podium: one craft on stage. The one it replaced stays for the length of
        // the swap, sinking and spinning away, so the change is a handover rather
        // than a cut.
        for (std::size_t i = 0; i < m_craft.size(); ++i) {
            Entity* e = nullptr;
            for (Entity& x : entities) if (x.id == m_craft[i].id) { e = &x; break; }
            if (!e) continue;
            const bool isSel  = static_cast<int>(i) == sel;
            const bool isPrev = static_cast<int>(i) == m_prevCraft && m_swap > 0.0f;
            e->active = isSel || isPrev;
            if (!e->active) continue;
            float y, yaw;
            if (isSel) {
                // Rises into place: below the podium at the start of the swap.
                const float rise = easeOut(1.0f - m_swap);
                y   = m_riseHeight * rise + bob * rise - 3.2f * (1.0f - rise);
                yaw = m_spin + 220.0f * (1.0f - rise);
            } else {
                y   = m_riseHeight - 4.0f * (1.0f - m_swap);
                yaw = m_spin - 300.0f * (1.0f - m_swap);
            }
            const glm::vec3 p = m_podium + glm::vec3(0.0f, y, 0.0f);
            if (m_craft[i].isRoot) { e->localCenter = p; e->center = p; }
            e->localRotation = glm::vec3(0.0f, yaw, 0.0f);
            e->rotation      = e->localRotation;
        }
    }
    if (m_swap <= 0.0f) m_prevCraft = -1;

    // The circuit strip slides so the chosen card stays at the centre.
    m_stripPos = approach(m_stripPos, static_cast<float>(m_trackSel), 11.0f, dt);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

fitzel::Texture* Showroom::preview(fitzel::AssetDatabase& assets,
                                   const std::string& path) {
    if (path.empty()) return nullptr;
    auto it = m_texCache.find(path);
    if (it == m_texCache.end()) {
        // Cache even a failed load, so a wrong path isn't retried every frame.
        it = m_texCache.emplace(path, assets.loadTexture(std::filesystem::path(path))).first;
    }
    return it->second ? it->second.get() : nullptr;
}

Launch Showroom::draw(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
                      fitzel::AssetDatabase& assets, const glm::mat4& viewProj) {
    Launch out;
    if (!m_active || !dl) return out;
    // The way out was asked for in update(); reported here so the caller reads
    // exactly one answer per frame, from one place.
    if (m_wantBack) { m_wantBack = false; out.back = true; return out; }

    Ctx g;
    g.dl   = dl;
    g.font = ImGui::GetFont();
    g.x0 = vmin.x;             g.y0 = vmin.y;
    g.x1 = vmin.x + vsize.x;   g.y1 = vmin.y + vsize.y;
    g.cx = (g.x0 + g.x1) * 0.5f;
    g.cy = (g.y0 + g.y1) * 0.5f;
    g.S  = std::clamp(vsize.y / 900.0f, 0.55f, 2.2f);
    g.fTiny = 12.0f * g.S; g.fSmall = 15.0f * g.S; g.fBody = 19.0f * g.S;
    g.fBig  = 38.0f * g.S; g.fHuge  = 74.0f * g.S;
    g.t     = m_time;
    g.acc   = accent();
    g.mouse = !ImGui::GetIO().WantCaptureMouse && !m_launching;

    const float pad   = 34.0f * g.S;
    const float intro = easeOut(m_intro);
    const float A     = intro * (1.0f - easeInOut(m_launch) * 0.85f); // master alpha

    // --- Where the podium is on screen -------------------------------------
    // The stage effects hang off the craft itself rather than a guessed point,
    // so they stay put while the camera orbits.
    float px = g.cx, py = g.cy + 40.0f * g.S;
    {
        const glm::vec4 clip = viewProj * glm::vec4(m_podium, 1.0f);
        if (clip.w > 0.001f) {
            px = g.x0 + (clip.x / clip.w * 0.5f + 0.5f) * vsize.x;
            py = g.y0 + (0.5f - clip.y / clip.w * 0.5f) * vsize.y;
        }
    }

    // --- Backdrop -----------------------------------------------------------
    // Cinematic bands top and bottom: the picker needs a dark ground to read
    // against, while the middle of the frame stays clear for the craft.
    const float topH = vsize.y * 0.18f, botH = vsize.y * 0.46f;
    dl->AddRectFilledMultiColor(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y0 + topH),
                                fade(IM_COL32(4, 6, 11, 235), A),
                                fade(IM_COL32(4, 6, 11, 235), A),
                                fade(IM_COL32(4, 6, 11, 0), A),
                                fade(IM_COL32(4, 6, 11, 0), A));
    dl->AddRectFilledMultiColor(ImVec2(g.x0, g.y1 - botH), ImVec2(g.x1, g.y1),
                                fade(IM_COL32(3, 5, 9, 0), A),
                                fade(IM_COL32(3, 5, 9, 0), A),
                                fade(IM_COL32(3, 5, 9, 245), A),
                                fade(IM_COL32(3, 5, 9, 245), A));
    // Side vignettes, tinted in the craft's accent so the whole frame takes its
    // colour the moment a different craft comes on stage.
    const float sideW = vsize.x * 0.30f;
    dl->AddRectFilledMultiColor(ImVec2(g.x0, g.y0), ImVec2(g.x0 + sideW, g.y1),
                                g.accent(0.10f * A), fade(IM_COL32(0, 0, 0, 0), A),
                                fade(IM_COL32(0, 0, 0, 0), A), g.accent(0.10f * A));
    dl->AddRectFilledMultiColor(ImVec2(g.x1 - sideW, g.y0), ImVec2(g.x1, g.y1),
                                fade(IM_COL32(0, 0, 0, 0), A), g.accent(0.10f * A),
                                g.accent(0.10f * A), fade(IM_COL32(0, 0, 0, 0), A));

    // --- Stage: the podium's holo rings -------------------------------------
    {
        const float base = vsize.y * 0.15f;
        for (int i = 0; i < 3; ++i) {
            const float k = 1.0f + i * 0.42f;
            const float wob = 1.0f + 0.03f * std::sin(m_time * (1.1f + i * 0.4f));
            dl->AddEllipse(ImVec2(px, py), ImVec2(base * k * wob, base * k * 0.30f),
                           g.accent((0.30f - i * 0.07f) * A),
                           m_time * (6.0f + i * 4.0f), 64, 1.6f * g.S);
        }
        // A soft pool of light under the craft.
        for (int i = 5; i >= 1; --i)
            dl->AddEllipseFilled(ImVec2(px, py),
                                 ImVec2(base * 0.9f * i * 0.28f, base * 0.27f * i * 0.28f),
                                 g.accent(0.035f * A), 0.0f, 48);
    }
    // The swap shockwave: a ring thrown off the podium the instant a craft changes.
    if (m_swap > 0.0f) {
        const float k = 1.0f - m_swap;                 // 0 at the change, 1 when done
        const float r = vsize.y * (0.10f + easeOut(k) * 0.85f);
        const float a = (1.0f - k) * (1.0f - k) * A;
        dl->AddEllipse(ImVec2(px, py), ImVec2(r, r * 0.34f), g.accent(0.75f * a),
                       0.0f, 72, 3.0f * g.S);
        dl->AddEllipse(ImVec2(px, py), ImVec2(r * 0.72f, r * 0.24f),
                       g.accent(0.40f * a), 0.0f, 72, 2.0f * g.S);
        // Sparks flung out with it.
        for (int i = 0; i < 26; ++i) {
            const float ang = (i / 26.0f) * 6.28318f + seeded(0x51F3u, i) * 0.4f;
            const float rr = r * (0.75f + seeded(0x9E37u, i) * 0.55f);
            const ImVec2 p0(px + std::cos(ang) * rr * 0.86f,
                            py + std::sin(ang) * rr * 0.34f * 0.86f);
            const ImVec2 p1(px + std::cos(ang) * rr,
                            py + std::sin(ang) * rr * 0.34f);
            dl->AddLine(p0, p1, g.accent(a * 0.9f), 2.0f * g.S);
        }
        // ...and a flash across the whole frame, brief enough to be felt more
        // than seen.
        if (m_swap > 0.75f)
            dl->AddRectFilled(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y1),
                              g.accent((m_swap - 0.75f) * 0.5f * A));
    }

    // --- Top bar ------------------------------------------------------------
    {
        const float ty = g.y0 + pad * 0.7f - (1.0f - intro) * 40.0f * g.S;
        if (!m_subtitle.empty())
            g.spaced(g.x0 + pad, ty, g.fSmall, g.accent(0.9f * A), m_subtitle.c_str(), 0.42f);
        if (!m_title.empty())
            g.text(g.x0 + pad, ty + g.fSmall * 1.9f, g.fBig, fade(kText, A), m_title.c_str());
        g.textR(g.x1 - pad, ty, g.fSmall, fade(kDim, A * 0.9f),
                "ESC  LEAVE       ARROWS  BROWSE       ENTER  CONFIRM");
        const float ly = ty + g.fSmall * 1.6f;
        dl->AddLine(ImVec2(g.x1 - pad - 300.0f * g.S, ly), ImVec2(g.x1 - pad, ly),
                    fade(kEdge, A), 1.0f);
    }

    // --- Craft panel (left) -------------------------------------------------
    const bool haveCraft = !m_craft.empty();
    const float panelW = std::min(vsize.x * 0.34f, 420.0f * g.S);
    const float panelX = g.x0 + pad - (1.0f - intro) * 90.0f * g.S;
    float panelY = g.cy - 150.0f * g.S;
    if (haveCraft) {
        const Craft& c = m_craft[m_craftSel];
        const float lift = easeOut(1.0f - m_swap);     // the block settles in on a swap
        const float ca   = A * (0.25f + 0.75f * lift);
        const float yoff = (1.0f - lift) * 26.0f * g.S;

        g.spaced(panelX, panelY - 26.0f * g.S + yoff, g.fTiny,
                 fade(kDim, ca), "CRAFT", 0.5f);
        // The name, with a brief digital stutter as it arrives.
        {
            const float jitter = m_swap > 0.55f
                ? (seeded(hashName(c.title), static_cast<int>(m_time * 60.0f) % 32) - 0.5f) *
                  14.0f * g.S * (m_swap - 0.55f) / 0.45f
                : 0.0f;
            const char* nm = c.title.c_str();
            if (std::abs(jitter) > 0.2f) {
                g.text(panelX + jitter, panelY + yoff, g.fBig,
                       IM_COL32(255, 90, 120, static_cast<int>(140 * ca)), nm);
                g.text(panelX - jitter, panelY + yoff, g.fBig,
                       g.accent(0.55f * ca), nm);
            }
            g.text(panelX, panelY + yoff, g.fBig, fade(kText, ca), nm);
        }
        float y = panelY + g.fBig * 1.15f + yoff;
        if (!c.team.empty()) {
            g.spaced(panelX, y, g.fSmall, g.accent(0.95f * ca), c.team.c_str(), 0.34f);
            y += g.fSmall * 2.0f;
        }
        if (!c.blurb.empty()) {
            g.text(panelX, y, g.fSmall, fade(kDim, ca * 0.95f), c.blurb.c_str());
            y += g.fSmall * 2.2f;
        }
        y += 6.0f * g.S;

        // Telemetry.
        const float bw = panelW - 20.0f * g.S, bh = 8.0f * g.S;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f KM/H", c.topSpeedMps * 3.6f);
        statBar(g, panelX, y, bw, bh, "TOP SPEED", m_bar[0], buf, ca);
        y += 40.0f * g.S;
        statBar(g, panelX, y, bw, bh, "THRUST",  m_bar[1], nullptr, ca); y += 40.0f * g.S;
        statBar(g, panelX, y, bw, bh, "GRIP",    m_bar[2], nullptr, ca); y += 40.0f * g.S;
        statBar(g, panelX, y, bw, bh, "AGILITY", m_bar[3], nullptr, ca);
        y += 46.0f * g.S;

        // Position in the list, as dots -- large enough to be countable at a
        // glance and clickable one by one.
        const float dotR = 6.0f * g.S, dotGap = 22.0f * g.S;
        for (std::size_t i = 0; i < m_craft.size(); ++i) {
            const ImVec2 dc(panelX + dotR + i * dotGap, y);
            const bool on = static_cast<int>(i) == m_craftSel;
            dl->AddCircleFilled(dc, on ? dotR : dotR * 0.55f,
                                on ? g.accent(A) : fade(kDim, A * 0.55f), 20);
            if (on) dl->AddCircle(dc, dotR * 2.0f, g.accent(A * 0.45f), 24, 1.5f * g.S);
            const ImVec2 m = ImGui::GetIO().MousePos;
            if (g.mouse && std::abs(m.x - dc.x) < dotGap * 0.5f &&
                std::abs(m.y - dc.y) < dotGap * 0.5f &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !on) {
                m_prevCraft = m_craftSel;
                m_orbitVel += (static_cast<int>(i) < m_craftSel ? 130.0f : -130.0f);
                m_craftSel  = static_cast<int>(i);
                m_swap      = 1.0f;
                m_row       = Row::Craft;
                cue(m_sndSelect.empty() ? m_sndMove : m_sndSelect, 1.0f);
            }
        }
    } else {
        g.textC(g.cx, g.cy, g.fBody, fade(kDim, A),
                "This showroom has no craft -- add a Glider to the scene.");
    }

    // --- Craft chevrons, flanking the stage ---------------------------------
    if (m_craft.size() > 1) {
        const float chY = py - vsize.y * 0.06f;
        const float chS = 30.0f * g.S;
        const bool armed = m_row == Row::Craft;
        if (chevron(g, px - vsize.x * 0.26f, chY, chS, -1, A, armed)) { moveCraft(-1); m_row = Row::Craft; }
        if (chevron(g, px + vsize.x * 0.26f, chY, chS,  1, A, armed)) { moveCraft( 1); m_row = Row::Craft; }
    }

    // --- Circuit strip ------------------------------------------------------
    const float cardW = 210.0f * g.S, cardH = 132.0f * g.S, cardGap = 18.0f * g.S;
    const float stripY = g.y1 - 118.0f * g.S - cardH + (1.0f - intro) * 120.0f * g.S;
    {
        g.spaced(g.x0 + pad, stripY - 26.0f * g.S, g.fTiny, fade(kDim, A), "CIRCUIT", 0.5f);
        if (m_tracks.empty()) {
            g.text(g.x0 + pad, stripY + cardH * 0.4f, g.fBody, fade(kDim, A),
                   "No circuits found -- add a Track Entry, or another scene to the project.");
        }
        // The strip is clipped to the viewport so cards sliding in from the sides
        // never bleed over the editor's panels.
        dl->PushClipRect(ImVec2(g.x0, stripY - 34.0f * g.S),
                         ImVec2(g.x1, stripY + cardH + 6.0f * g.S), true);
        for (std::size_t i = 0; i < m_tracks.size(); ++i) {
            const Track& t = m_tracks[i];
            const bool on = static_cast<int>(i) == m_trackSel;
            const float rel = static_cast<float>(i) - m_stripPos;
            const float ax = g.cx + rel * (cardW + cardGap) - cardW * 0.5f;
            if (ax > g.x1 + cardW || ax + cardW < g.x0 - cardW) continue;
            // Cards away from the centre shrink and dim, so the strip reads as
            // depth rather than as a row of equals.
            const float k  = clamp01(1.0f - std::abs(rel) * 0.30f);
            const float sc = 0.80f + 0.20f * k;
            const float ay = stripY + cardH * (1.0f - sc) * 0.5f;
            const float bx = ax + cardW * sc, by = ay + cardH * sc;
            const float ca = A * (0.32f + 0.68f * k);
            const float r  = 7.0f * g.S;

            dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by), fade(kInk, ca), r);
            // Preview art: the authored image, or a circuit glyph generated from
            // the name so an unillustrated project still looks finished.
            if (fitzel::Texture* tex = preview(assets, t.image)) {
                dl->AddImageRounded((ImTextureID)(std::intptr_t)tex->id(),
                                    ImVec2(ax, ay), ImVec2(bx, by),
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    IM_COL32(255, 255, 255, static_cast<int>(255 * ca)), r);
                dl->AddRectFilledMultiColor(ImVec2(ax, ay + (by - ay) * 0.35f), ImVec2(bx, by),
                                            fade(IM_COL32(4, 6, 11, 0), ca),
                                            fade(IM_COL32(4, 6, 11, 0), ca),
                                            fade(IM_COL32(4, 6, 11, 240), ca),
                                            fade(IM_COL32(4, 6, 11, 240), ca));
            } else {
                circuitGlyph(g, ax, ay - cardH * 0.10f, bx, by - cardH * 0.10f,
                             hashName(t.scene),
                             on ? g.accent(0.85f * ca) : fade(kDim, ca * 0.7f),
                             on ? 2.4f * g.S : 1.6f * g.S,
                             on ? m_time * 0.12f : 0.0f);
            }
            dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by),
                        on ? g.accent(0.95f * ca) : fade(kEdge, ca), r, 0,
                        on ? 2.4f * g.S : 1.0f);
            // The chosen card catches a light sweep, restarted on every change.
            if (on && m_trackFx > 0.0f) {
                const float sweep = (1.0f - m_trackFx) * (bx - ax + 90.0f * g.S) - 45.0f * g.S;
                dl->PushClipRect(ImVec2(ax, ay), ImVec2(bx, by), true);
                dl->AddRectFilledMultiColor(
                    ImVec2(ax + sweep - 40.0f * g.S, ay), ImVec2(ax + sweep + 40.0f * g.S, by),
                    g.accent(0.0f), g.accent(0.30f * m_trackFx * ca),
                    g.accent(0.30f * m_trackFx * ca), g.accent(0.0f));
                dl->PopClipRect();
            }

            const float tx = ax + 12.0f * g.S;
            g.text(tx, by - 44.0f * g.S, g.fBody * sc,
                   on ? fade(kText, ca) : fade(kDim, ca), t.title.c_str());
            char meta[48];
            if (t.lengthKm > 0.0f && t.laps > 0)
                std::snprintf(meta, sizeof(meta), "%d LAPS  -  %.2f KM", t.laps, t.lengthKm);
            else if (t.laps > 0)  std::snprintf(meta, sizeof(meta), "%d LAPS", t.laps);
            else if (t.lengthKm > 0.0f) std::snprintf(meta, sizeof(meta), "%.2f KM", t.lengthKm);
            else                  std::snprintf(meta, sizeof(meta), "CIRCUIT");
            g.spaced(tx, by - 22.0f * g.S, g.fTiny * sc, fade(kDim, ca), meta, 0.24f);
            pips(g, bx - 12.0f * g.S - 5 * 5.0f * g.S * 1.7f, by - 22.0f * g.S,
                 5.0f * g.S, t.difficulty, ca);

            const ImVec2 m = ImGui::GetIO().MousePos;
            if (g.mouse && m.x >= ax && m.x <= bx && m.y >= ay && m.y <= by &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                m_row = Row::Track;
                if (!on) { m_trackSel = static_cast<int>(i); m_trackFx = 1.0f; cue(m_sndMove, 1.06f); }
            }
        }
        dl->PopClipRect();
        // The chosen circuit's sentence, under the strip.
        if (!m_tracks.empty() && !m_tracks[m_trackSel].blurb.empty())
            g.textC(g.cx, stripY + cardH + 10.0f * g.S, g.fSmall, fade(kDim, A * 0.9f),
                    m_tracks[m_trackSel].blurb.c_str());
    }

    // --- START --------------------------------------------------------------
    {
        const float bw = 320.0f * g.S, bh = 62.0f * g.S;
        const float ax = g.cx - bw * 0.5f, ay = g.y1 - 88.0f * g.S;
        const float pulse = 0.5f + 0.5f * std::sin(m_time * 3.4f);
        const bool  ready = haveCraft && !m_tracks.empty();
        int hovered = 0;
        const bool clicked =
            bigButton(g, ax, ay, ax + bw, ay + bh, "START RACE",
                      m_row == Row::Start && ready, A * (ready ? 1.0f : 0.35f),
                      &hovered, pulse);
        if (hovered) m_row = Row::Start;
        // Chevrons converging on the button while it is armed: the one piece of
        // motion that says "this is the thing that commits".
        if (ready && m_row == Row::Start && !m_launching) {
            const float k = std::fmod(m_time * 1.6f, 1.0f);
            for (int s = -1; s <= 1; s += 2) {
                const float dx = (1.0f - k) * 40.0f * g.S;
                const float cxx = (s < 0 ? ax - 22.0f * g.S - dx : ax + bw + 22.0f * g.S + dx);
                const float cyy = ay + bh * 0.5f, sz = 9.0f * g.S;
                dl->AddLine(ImVec2(cxx - s * sz * 0.5f, cyy - sz),
                            ImVec2(cxx + s * sz * 0.5f, cyy), g.accent(k * A), 2.4f * g.S);
                dl->AddLine(ImVec2(cxx + s * sz * 0.5f, cyy),
                            ImVec2(cxx - s * sz * 0.5f, cyy + sz), g.accent(k * A), 2.4f * g.S);
            }
        }
        if (ready && !m_launching && clicked) {
            m_launching = true; m_launch = 0.0f;
            cue(m_sndStart, 1.0f);
        }
    }

    // --- Launch wipe --------------------------------------------------------
    if (m_launching) {
        const float k = easeInOut(m_launch);
        // Speed lines converging on the podium, then a wash of the craft's colour.
        for (int i = 0; i < 48; ++i) {
            const float ang = (i / 48.0f) * 6.28318f + seeded(0xA53Cu, i) * 0.3f;
            const float len = vsize.y * (0.25f + seeded(0x1B7Fu, i) * 0.9f) * k;
            const float r0 = vsize.y * 0.16f + len * 0.15f;
            dl->AddLine(ImVec2(px + std::cos(ang) * r0, py + std::sin(ang) * r0),
                        ImVec2(px + std::cos(ang) * (r0 + len),
                               py + std::sin(ang) * (r0 + len)),
                        g.accent(0.55f * k), 2.0f * g.S);
        }
        if (k > 0.45f) {
            const float w = (k - 0.45f) / 0.55f;
            dl->AddRectFilled(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y1),
                              rgba(glm::mix(accent(), glm::vec3(1.0f), w), w));
        }
        const float fs = g.fBig * (1.0f + k * 0.35f);
        g.textC(g.cx, g.cy - fs * 0.5f, fs,
                fade(IM_COL32(255, 255, 255, 255), clamp01(1.0f - k * 1.6f)), "LAUNCHING");
        if (m_launch >= 1.0f) {
            out.start = true;
            out.scene = m_tracks.empty() ? std::string() : m_tracks[m_trackSel].scene;
            if (haveCraft) {
                out.craftId   = m_craft[m_craftSel].id;
                out.craftName = m_craft[m_craftSel].title;
            }
            out.laps = m_tracks.empty() ? 0 : m_tracks[m_trackSel].laps;
        }
        return out;
    }

    // --- Entry wipe ---------------------------------------------------------
    if (m_intro < 1.0f) {
        const float k = 1.0f - intro;
        dl->AddRectFilled(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y0 + vsize.y * 0.5f * k),
                          IM_COL32(3, 5, 9, static_cast<int>(240 * k)));
        dl->AddRectFilled(ImVec2(g.x0, g.y1 - vsize.y * 0.5f * k), ImVec2(g.x1, g.y1),
                          IM_COL32(3, 5, 9, static_cast<int>(240 * k)));
    }
    return out;
}

} // namespace showroom
