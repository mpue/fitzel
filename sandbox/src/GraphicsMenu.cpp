#include "GraphicsMenu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#include <fitzel/render/Renderer.hpp>

namespace gfxmenu {
namespace {

// --- The rows ---------------------------------------------------------------
// One table for the whole menu: a row's label, the field it steps, the names of
// its steps, and what the player is told about it. Keeping the four together is
// what stops a row from growing a step its label does not describe -- and the
// VALUES the steps mean live in the small tables further down, next to the code
// that applies them, so there is exactly one place per setting to look.

const char* kViewOpts[]    = {"Minimum", "Very low", "Low", "Medium",
                              "High", "Very high", "Ultra", "Maximum", nullptr};
const char* kShadowOpts[]  = {"Off", "Low", "Medium", "High", nullptr};
const char* kReflectOpts[] = {"Off", "Low", "Medium", "High", "Ultra", nullptr};
const char* kRateOpts[]    = {"Lazy", "Balanced", "Quick", "Instant", nullptr};
const char* kVegOpts[]     = {"Off", "Sparse", "Medium", "Full", nullptr};
const char* kCityOpts[]    = {"Near", "Balanced", "Far", nullptr};
const char* kOffOn[]       = {"Off", "On", nullptr};
const char* kBlurOpts[]    = {"Off", "Subtle", "Full", nullptr};
const char* kAaOpts[]      = {"Off", "FXAA", nullptr};

struct Row {
    const char* label;
    int Settings::* field;
    const char* const* opts;
    const char* help;   // what it changes about the picture
    const char* cost;   // and what that costs, in the terms that matter here
};

const Row kRows[] = {
    {"View distance", &Settings::viewDistance, kViewOpts,
     "How much landscape is streamed in and drawn around you. The lowest steps\n"
     "bring the horizon close enough to see the ground end.",
     "The heaviest setting on this screen: everything in view is drawn again\n"
     "for every shadow cascade and for the reflection probe."},

    {"Shadows", &Settings::shadows, kShadowOpts,
     "Resolution of the sun's shadow cascades -- how crisp a shadow edge is,\n"
     "and how far out things still cast one. Off leaves the scene lit flat.",
     "Off skips four whole passes over the scene and is the single biggest\n"
     "win on a weak machine. High costs memory more than frame time."},

    {"Reflections", &Settings::reflections, kReflectOpts,
     "Size of the cube the world is captured into for wet roads, water and\n"
     "polished materials. Off leaves them with the sun's sheen only.",
     "Off is free. The steps above cost fill rate, not draw calls -- Ultra is\n"
     "sixty-four times the pixels of Low."},

    {"Reflection refresh", &Settings::reflectRate, kRateOpts,
     "How quickly that cube keeps up. Lazy fills one face per frame, so a\n"
     "reflection is a few frames -- metres, at racing speed -- behind you.",
     "Only spent while the view actually moves: a parked camera pays the\n"
     "lazy rate whatever this says."},

    {"Vegetation", &Settings::vegetation, kVegOpts,
     "Grass and flowers on the ground beside the road. Off leaves the bare\n"
     "terrain texture; the steps raise both how dense it is and how far.",
     "Instanced, so it is fill rate rather than draw calls -- which makes it\n"
     "the row to try first when the frame rate drops in close-up shots."},

    {"City detail", &Settings::cityDetail, kCityOpts,
     "When roadside buildings stop being drawn. Near drops anything small on\n"
     "screen early; Far keeps the skyline out to the road's own draw range.",
     "A building costs the same draw whether it is four pixels tall or four\n"
     "hundred, which is exactly what Near is there to stop paying for."},

    {"Ambient occlusion", &Settings::ao, kOffOn,
     "The contact shading in creases, under kerbs and where objects meet the\n"
     "ground. Off flattens the picture but changes nothing else.",
     "One full-screen pass. Steady cost, independent of what is in view."},

    {"Bloom and sun rays", &Settings::bloom, kOffOn,
     "The glow around bright things and the shafts of light past the sun.\n"
     "On restores whatever the scene's author set, not a fixed amount.",
     "A small pyramid of half-size passes. Cheap, but it is pure fill rate."},

    {"Depth of field", &Settings::dof, kOffOn,
     "Blurs what is far outside the focal range. Off keeps the whole depth\n"
     "of the image sharp.",
     "One blur pass at full resolution."},

    {"Motion blur", &Settings::motionBlur, kBlurOpts,
     "The radial streak that grows past the craft with speed. Subtle halves\n"
     "the authored strength; Off leaves the image still.",
     "One pass, and only while something is being driven or flown."},

    {"Anti-aliasing", &Settings::aa, kAaOpts,
     "Smooths the stair-stepping along edges. FXAA works on the finished\n"
     "image, so it softens fine detail slightly along with the edges.",
     "One cheap full-screen pass."},

    {"V-Sync", &Settings::vsync, kOffOn,
     "Hands each finished frame to the screen on its own refresh. On removes\n"
     "tearing; off lets the frame rate run past the refresh rate.",
     "Off can feel more responsive and will show the true frame rate, which\n"
     "is what makes it worth having while tuning this screen."},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

int optionCount(const char* const* opts) {
    int n = 0;
    while (opts[n]) ++n;
    return n;
}

// --- Presets ----------------------------------------------------------------
// Written out per row rather than derived, because a preset is a JUDGEMENT about
// what to give up first on a weak machine, and there is no formula for that:
// Low keeps anti-aliasing (a cheap pass that costs nothing and saves the image)
// and throws away shadows (four passes over the scene) even though "quality"
// would rank them the other way round.
const Settings kPresets[4] = {
    // preset, view, shadow, refl, rate, veg, city, ao, bloom, dof, mb, aa, vsync
    {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1},   // Low
    {1, 2, 1, 1, 1, 2, 1, 1, 1, 0, 1, 1, 1},   // Medium
    {2, 3, 2, 2, 2, 3, 1, 1, 1, 1, 2, 1, 1},   // High
    {3, 5, 3, 3, 3, 3, 2, 1, 1, 1, 2, 1, 1},   // Ultra
};
const char* kPresetNames[] = {"Low", "Medium", "High", "Ultra"};
constexpr int kCustom = 4;

// --- What the steps mean ----------------------------------------------------
const int   kViewRadius[]   = {2, 3, 4, 5, 6, 7, 8, 9};
const int   kShadowRes[]    = {1024, 1024, 2048, 4096};  // [0] unused: shadows off
const int   kProbeRes[]     = {128, 128, 256, 512, 1024}; // [0] unused: probe off
const int   kProbeFaces[]   = {1, 2, 3, 6};
const float kGrassDensity[] = {0.0f, 0.55f, 1.0f, 1.6f};
const float kGrassRadius[]  = {20.0f, 30.0f, 46.0f, 68.0f};
const float kCityMinPx[]    = {16.0f, 7.0f, 2.5f};

// --- Colour and drawing -----------------------------------------------------
// The same ink the showroom and the race screens are drawn in, so this does not
// read as a different application's dialog dropped into the game.
constexpr ImU32 kInk   = IM_COL32(  8,  11,  17, 214);
constexpr ImU32 kEdge  = IM_COL32(255, 255, 255,  30);
constexpr ImU32 kText  = IM_COL32(238, 242, 249, 255);
constexpr ImU32 kDim   = IM_COL32(146, 158, 178, 255);

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float easeOut(float t) { const float u = 1.0f - clamp01(t); return 1.0f - u * u * u; }

ImU32 fade(ImU32 col, float a) {
    const float f = clamp01(a);
    return (col & 0x00FFFFFFu) |
           (static_cast<ImU32>(((col >> 24) & 0xFF) * f) << 24);
}
ImU32 accent(float a) {
    return IM_COL32(90, 214, 255, static_cast<int>(clamp01(a) * 255.0f));
}

struct Ctx {
    ImDrawList* dl   = nullptr;
    ImFont*     font = nullptr;
    float S = 1.0f;
    float fTiny = 0, fSmall = 0, fBody = 0, fBig = 0, fHuge = 0;
    float t = 0.0f;
    bool  mouse = true;

    float textW(float size, const char* s) const {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    }
    void text(float x, float y, float size, ImU32 col, const char* s) const {
        const float o = std::max(1.0f, size * 0.06f);
        dl->AddText(font, size, ImVec2(x + o, y + o),
                    fade(IM_COL32(0, 0, 0, 200), ((col >> 24) & 0xFF) / 255.0f), s);
        dl->AddText(font, size, ImVec2(x, y), col, s);
    }
    void textC(float xm, float y, float size, ImU32 col, const char* s) const {
        text(xm - textW(size, s) * 0.5f, y, size, col, s);
    }
    void textR(float xr, float y, float size, ImU32 col, const char* s) const {
        text(xr - textW(size, s), y, size, col, s);
    }
    // Letter-spaced small caps, the game's screens' one typographic flourish.
    void spaced(float x, float y, float size, ImU32 col, const char* s,
                float extra = 0.26f) const {
        float cx = x;
        for (const char* p = s; *p; ++p) {
            const char b[2] = {*p, '\0'};
            text(cx, y, size, col, b);
            cx += textW(size, b) + size * extra;
        }
    }
    float spacedW(float size, const char* s, float extra = 0.26f) const {
        float w = 0.0f;
        for (const char* p = s; *p; ++p) {
            const char b[2] = {*p, '\0'};
            w += textW(size, b) + size * extra;
        }
        return w;
    }
    bool over(float ax, float ay, float bx, float by) const {
        const ImVec2 m = ImGui::GetIO().MousePos;
        return mouse && m.x >= ax && m.x <= bx && m.y >= ay && m.y <= by;
    }
};

void panel(const Ctx& g, float ax, float ay, float bx, float by, float alpha,
           bool bar = true) {
    const float r = 7.0f * g.S;
    g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by), fade(kInk, alpha), r);
    g.dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by), fade(kEdge, alpha), r, 0, 1.0f);
    if (bar)
        g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + 3.0f * g.S, by),
                            accent(0.85f * alpha), r, ImDrawFlags_RoundCornersLeft);
}

// A chevron step control. Deliberately oversized: this is the control that gets
// used most on this screen and it must not need a steady hand.
bool chevron(const Ctx& g, float cx, float cy, float size, int dir, float alpha,
             bool armed, bool* hovered) {
    const float hit = size * 1.15f;
    const bool  hot = g.over(cx - hit, cy - hit, cx + hit, cy + hit);
    if (hot && hovered) *hovered = true;
    const float a  = alpha * (hot ? 1.0f : (armed ? 0.85f : 0.30f));
    const float w = size * 0.40f, h = size * 0.58f;
    const float th = std::max(2.0f, size * 0.15f);
    if (hot)
        g.dl->AddCircleFilled(ImVec2(cx, cy), hit * 0.85f, accent(0.16f * alpha), 20);
    g.dl->AddLine(ImVec2(cx + dir * -w * 0.4f, cy - h),
                  ImVec2(cx + dir *  w * 0.6f, cy), accent(a), th);
    g.dl->AddLine(ImVec2(cx + dir *  w * 0.6f, cy),
                  ImVec2(cx + dir * -w * 0.4f, cy + h), accent(a), th);
    return hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

bool bigButton(const Ctx& g, float ax, float ay, float bx, float by,
               const char* label, bool lit, float alpha, float pulse) {
    const bool hot = g.over(ax, ay, bx, by);
    const bool on  = lit || hot;
    const float r = 8.0f * g.S;
    if (on)
        for (int i = 3; i >= 1; --i) {
            const float o = i * 3.0f * g.S * (1.0f + pulse * 0.5f);
            g.dl->AddRect(ImVec2(ax - o, ay - o), ImVec2(bx + o, by + o),
                          accent(0.10f * alpha / i), r + o, 0, 2.0f * g.S);
        }
    g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by),
                        on ? accent(0.20f * alpha) : fade(kInk, alpha), r);
    g.dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by),
                  on ? accent(0.95f * alpha) : fade(kEdge, alpha), r, 0,
                  on ? 2.2f * g.S : 1.0f);
    const float fs = g.fBody;
    const float lw = g.spacedW(fs, label, 0.16f);
    g.spaced((ax + bx) * 0.5f - lw * 0.5f, (ay + by) * 0.5f - fs * 0.62f, fs,
             on ? accent(alpha) : fade(kText, alpha), label, 0.16f);
    return hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// Draw a multi-line string (the help texts carry their own newlines, so they
// break where they read best rather than where a width happens to fall).
float paragraph(const Ctx& g, float x, float y, float size, ImU32 col,
                const char* s) {
    const float lh = size * 1.45f;
    const char* line = s;
    while (*line) {
        const char* nl = line;
        while (*nl && *nl != '\n') ++nl;
        char buf[256];
        const std::size_t n = std::min(sizeof(buf) - 1,
                                       static_cast<std::size_t>(nl - line));
        std::memcpy(buf, line, n);
        buf[n] = '\0';
        g.text(x, y, size, col, buf);
        y += lh;
        line = (*nl == '\n') ? nl + 1 : nl;
    }
    return y;
}

} // namespace

// ---------------------------------------------------------------------------

bool Settings::operator==(const Settings& o) const {
    // `preset` is deliberately excluded: it is a LABEL for the rest, so two
    // settings that differ only in what they are called are the same settings --
    // which is exactly what refreshPresetLabel needs to ask.
    return viewDistance == o.viewDistance && shadows == o.shadows &&
           reflections == o.reflections && reflectRate == o.reflectRate &&
           vegetation == o.vegetation && cityDetail == o.cityDetail &&
           ao == o.ao && bloom == o.bloom && dof == o.dof &&
           motionBlur == o.motionBlur && aa == o.aa && vsync == o.vsync;
}

void applyPreset(Settings& s, int preset) {
    if (preset < 0 || preset > 3) return;
    s = kPresets[preset];
}

void refreshPresetLabel(Settings& s) {
    for (int i = 0; i < 4; ++i)
        if (s == kPresets[i]) { s.preset = i; return; }
    s.preset = kCustom;
}

Settings load(const std::string& file) {
    Settings s;
    std::ifstream f(file);
    if (!f) return s;                       // no file yet: the built-in defaults
    nlohmann::json j;
    try { f >> j; } catch (...) { return s; }
    auto get = [&](const char* key, int def, const char* const* opts) {
        const int v = j.value(key, def);
        return std::clamp(v, 0, optionCount(opts) - 1);
    };
    s.viewDistance = get("viewDistance", s.viewDistance, kViewOpts);
    s.shadows      = get("shadows",      s.shadows,      kShadowOpts);
    s.reflections  = get("reflections",  s.reflections,  kReflectOpts);
    s.reflectRate  = get("reflectRate",  s.reflectRate,  kRateOpts);
    s.vegetation   = get("vegetation",   s.vegetation,   kVegOpts);
    s.cityDetail   = get("cityDetail",   s.cityDetail,   kCityOpts);
    s.ao           = get("ao",           s.ao,           kOffOn);
    s.bloom        = get("bloom",        s.bloom,        kOffOn);
    s.dof          = get("dof",          s.dof,          kOffOn);
    s.motionBlur   = get("motionBlur",   s.motionBlur,   kBlurOpts);
    s.aa           = get("aa",           s.aa,           kAaOpts);
    s.vsync        = get("vsync",        s.vsync,        kOffOn);
    refreshPresetLabel(s);
    return s;
}

void save(const std::string& file, const Settings& s) {
    const nlohmann::json j = {
        {"viewDistance", s.viewDistance}, {"shadows", s.shadows},
        {"reflections", s.reflections},   {"reflectRate", s.reflectRate},
        {"vegetation", s.vegetation},     {"cityDetail", s.cityDetail},
        {"ao", s.ao},                     {"bloom", s.bloom},
        {"dof", s.dof},                   {"motionBlur", s.motionBlur},
        {"aa", s.aa},                     {"vsync", s.vsync},
    };
    std::ofstream f(file);
    if (f) f << j.dump(2) << '\n';
}

void apply(const Settings& s, const Settings& prev, fitzel::Renderer& renderer,
           const Targets& t) {
    // "Nothing moved" can only mean the caller is pushing what is already in
    // force -- a startup, or a scene load -- because a menu edit always moves
    // something. That is the documented way to ask for the expensive follow-ups
    // (re-seeding the grass, the swap interval) to run unconditionally.
    const bool force = (prev == s);
    if (t.viewRadius) *t.viewRadius = kViewRadius[std::clamp(s.viewDistance, 0, 7)];

    renderer.setShadowsEnabled(s.shadows > 0);
    if (s.shadows > 0) renderer.shadows().setResolution(kShadowRes[s.shadows]);

    // The probe is switched off by handing it the smallest cube rather than by a
    // flag: nothing in the frame asks whether reflections are "on", it asks the
    // probe for a colour -- and a 128 cube that is never sharp enough to read is
    // both the cheapest honest answer and one that keeps a wet road wet.
    const int probe = kProbeRes[std::clamp(s.reflections, 0, 4)];
    if (t.envProbeRes && *t.envProbeRes != probe) {
        *t.envProbeRes = probe;
        renderer.setEnvProbeResolution(probe);
    }
    const int faces = kProbeFaces[std::clamp(s.reflectRate, 0, 3)];
    if (t.envProbeFaces && *t.envProbeFaces != faces) {
        *t.envProbeFaces = faces;
        renderer.setEnvProbeMaxFaces(faces);
    }

    if (t.fxaa) *t.fxaa = (s.aa > 0);

    const int veg = std::clamp(s.vegetation, 0, 3);
    if (t.grassEnabled)  *t.grassEnabled  = (veg > 0);
    if (t.flowerEnabled) *t.flowerEnabled = (veg > 1);
    if (t.grassDensity)  *t.grassDensity  = std::max(kGrassDensity[veg], 0.1f);
    if (t.grassRadius)   *t.grassRadius   = kGrassRadius[veg];
    if (t.flowerDensity) *t.flowerDensity = (veg > 1) ? 1.0f : 0.0f;
    // Re-seeding is the one expensive thing on this list, so it is asked for only
    // when the row that changes the layout actually moved.
    if (t.regrowVegetation && (force || prev.vegetation != s.vegetation))
        t.regrowVegetation();

    if (t.cityMinPixels) *t.cityMinPixels = kCityMinPx[std::clamp(s.cityDetail, 0, 2)];

    if (t.setVSync && (force || prev.vsync != s.vsync)) t.setVSync(s.vsync > 0);
}

PostGate gatePost(const Settings& s, float ssaoStrength, float bloomIntensity,
                  float rayIntensity, float dofMax, float blurStrength) {
    PostGate g;
    g.ssaoStrength   = (s.ao > 0)    ? ssaoStrength   : 0.0f;
    g.bloomIntensity = (s.bloom > 0) ? bloomIntensity : 0.0f;
    g.rayIntensity   = (s.bloom > 0) ? rayIntensity   : 0.0f;
    g.dofMax         = (s.dof > 0)   ? dofMax         : 0.0f;
    g.blurStrength   = blurStrength * (s.motionBlur == 0 ? 0.0f
                                     : s.motionBlur == 1 ? 0.5f : 1.0f);
    return g;
}

// ---------------------------------------------------------------------------

void Menu::setOpen(bool v) {
    if (v == m_open) return;
    m_open = v;
    if (v) {
        m_intro = 0.0f;
        m_focus = 0;
        m_frames.clear();
    }
}

void Menu::update(float dt) {
    if (!m_open) { m_intro = 0.0f; return; }
    m_time  += dt;
    m_intro  = std::min(1.0f, m_intro + dt * 4.0f);
    if (dt > 0.0f) {
        m_frames.push_back(dt);
        if (m_frames.size() > 160) m_frames.erase(m_frames.begin());
        // Smoothed hard, because an unsmoothed figure flickers too much to read
        // and this screen exists to be read while something else is changed.
        const float fps = 1.0f / std::max(dt, 1e-4f);
        m_fpsShown = (m_fpsShown <= 0.0f) ? fps
                                          : m_fpsShown + (fps - m_fpsShown) * 0.08f;
    }
}

bool Menu::draw(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
                Settings& s, const Input& in, bool* changed) {
    if (!m_open || !dl) return false;
    if (changed) *changed = false;

    Ctx g;
    g.dl   = dl;
    g.font = ImGui::GetFont();
    g.S    = std::clamp(vsize.y / 900.0f, 0.55f, 2.2f);
    g.fTiny = 12.0f * g.S; g.fSmall = 15.0f * g.S; g.fBody = 19.0f * g.S;
    g.fBig  = 34.0f * g.S; g.fHuge  = 64.0f * g.S;
    g.t     = m_time;
    g.mouse = !ImGui::GetIO().WantCaptureMouse;

    const float A     = easeOut(m_intro);
    const float pulse = 0.5f + 0.5f * std::sin(m_time * 3.0f);

    const float x0 = vmin.x, y0 = vmin.y;
    const float x1 = vmin.x + vsize.x, y1 = vmin.y + vsize.y;

    // --- Scrim --------------------------------------------------------------
    // The game stays visible underneath on purpose: every row here changes what
    // is behind the panel, and a change nobody can see is a change nobody can
    // judge. Hence a scrim rather than a background.
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                      fade(IM_COL32(3, 5, 9, 205), A));

    const float pad = 30.0f * g.S;
    const float pw  = std::min(1240.0f * g.S, vsize.x - pad * 2.0f);
    const float ph  = std::min(880.0f * g.S, vsize.y - pad * 2.0f);
    const float ax  = (x0 + x1) * 0.5f - pw * 0.5f;
    const float ay  = (y0 + y1) * 0.5f - ph * 0.5f + (1.0f - A) * 18.0f * g.S;
    const float bx  = ax + pw, by = ay + ph;
    panel(g, ax, ay, bx, by, A, /*bar=*/false);
    dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, ay + 3.0f * g.S), accent(0.9f * A),
                      3.0f * g.S);

    // --- Header -------------------------------------------------------------
    const float ix = ax + 32.0f * g.S;          // inner left
    const float iw = pw - 64.0f * g.S;          // inner width
    float cy = ay + 26.0f * g.S;
    g.spaced(ix, cy, g.fBig, fade(kText, A), "GRAPHICS", 0.20f);
    g.textR(ix + iw, cy + g.fBig * 0.25f, g.fSmall, fade(kDim, A),
            "Fit the game to this machine. Everything changes as you choose it.");
    cy += g.fBig * 1.9f;

    // --- Preset chips -------------------------------------------------------
    {
        const float chipW = (iw - 40.0f * g.S) / 5.0f;
        const float chipH = 46.0f * g.S;
        for (int i = 0; i < 4; ++i) {
            const float cx = ix + i * (chipW + 10.0f * g.S);
            if (bigButton(g, cx, cy, cx + chipW, cy + chipH, kPresetNames[i],
                          s.preset == i, A, pulse)) {
                applyPreset(s, i);
                if (changed) *changed = true;
            }
        }
        // The fifth slot is a readout, not a button: "custom" is something the
        // settings BECOME by being edited, never something to pick.
        const float cx = ix + 4 * (chipW + 10.0f * g.S);
        const bool  cus = (s.preset == kCustom);
        dl->AddRect(ImVec2(cx, cy), ImVec2(cx + chipW, cy + chipH),
                    fade(cus ? kEdge : IM_COL32(255, 255, 255, 12), A), 8.0f * g.S,
                    0, 1.0f);
        const float lw = g.spacedW(g.fBody, "Custom", 0.16f);
        g.spaced(cx + chipW * 0.5f - lw * 0.5f, cy + chipH * 0.5f - g.fBody * 0.62f,
                 g.fBody, fade(cus ? kText : kDim, cus ? A : A * 0.45f), "Custom",
                 0.16f);
        cy += chipH + 22.0f * g.S;
    }

    // --- Rows (left) and the info column (right) ---------------------------
    const float colGap  = 26.0f * g.S;
    const float infoW   = std::min(400.0f * g.S, iw * 0.34f);
    const float rowsW   = iw - infoW - colGap;
    const float rowsTop = cy;
    const float botH    = 78.0f * g.S;                  // the bar at the bottom
    const float rowsBot = by - botH;
    const float rowH    = std::min(52.0f * g.S,
                                   (rowsBot - rowsTop) / static_cast<float>(kRowCount));

    int   hoverRow = -1;
    bool  onChevron = false;
    int   step = 0;          // -1 / +1 applied to the focused row after the loop
    // A step that WRAPS (a click, Enter) versus one that stops at the end (an
    // arrow key, a chevron): the first needs a way past the last option because
    // it is the only control it has, the second has the opposite direction
    // sitting right next to it and a stop is the honest answer there.
    bool  stepWraps = false;

    for (int i = 0; i < kRowCount; ++i) {
        const Row&  r  = kRows[i];
        const float ry = rowsTop + i * rowH;
        const float rb = ry + rowH - 4.0f * g.S;
        const bool  foc = (i == m_focus);
        const bool  hot = g.over(ix, ry, ix + rowsW, rb);
        if (hot) hoverRow = i;

        if (foc) {
            dl->AddRectFilled(ImVec2(ix, ry), ImVec2(ix + rowsW, rb),
                              accent(0.13f * A), 6.0f * g.S);
            dl->AddRectFilled(ImVec2(ix, ry), ImVec2(ix + 3.0f * g.S, rb),
                              accent(0.95f * A), 2.0f * g.S);
        } else if (hot) {
            dl->AddRectFilled(ImVec2(ix, ry), ImVec2(ix + rowsW, rb),
                              fade(IM_COL32(255, 255, 255, 14), A), 6.0f * g.S);
        }

        const float ty = (ry + rb) * 0.5f - g.fBody * 0.62f;
        g.text(ix + 16.0f * g.S, ty, g.fBody,
               fade(foc ? kText : kDim, A * (foc ? 1.0f : 0.85f)), r.label);

        // Value block: chevron, name, chevron -- right-aligned, fixed width, so
        // the eye runs down a column of values rather than a ragged edge.
        const int  n   = optionCount(r.opts);
        const int  cur = std::clamp(s.*(r.field), 0, n - 1);
        const float vw  = 190.0f * g.S;
        const float vx0 = ix + rowsW - vw;
        const float vcy = (ry + rb) * 0.5f;
        if (chevron(g, vx0 + 16.0f * g.S, vcy, 15.0f * g.S, -1, A, foc && cur > 0,
                    &onChevron) && cur > 0) {
            s.*(r.field) = cur - 1; m_focus = i; if (changed) *changed = true;
        }
        if (chevron(g, vx0 + vw - 16.0f * g.S, vcy, 15.0f * g.S, +1, A,
                    foc && cur < n - 1, &onChevron) && cur < n - 1) {
            s.*(r.field) = cur + 1; m_focus = i; if (changed) *changed = true;
        }
        const bool offish = (cur == 0 && std::strcmp(r.opts[0], "Off") == 0);
        g.textC(vx0 + vw * 0.5f, ty, g.fBody,
                offish ? fade(kDim, A * 0.8f) : (foc ? accent(A) : fade(kText, A)),
                r.opts[cur]);

        // Step dots under the value: how many steps this row has and where this
        // one sits, which is the thing a name alone cannot say.
        if (n > 2) {
            const float dy = rb - 8.0f * g.S;
            const float dw = 7.0f * g.S;
            const float sx = vx0 + vw * 0.5f - (n - 1) * dw * 0.5f;
            for (int k = 0; k < n; ++k)
                dl->AddCircleFilled(ImVec2(sx + k * dw, dy), 1.6f * g.S,
                                    k == cur ? accent(A) : fade(kDim, A * 0.35f), 8);
        }
    }

    // Hovering a row focuses it, and clicking anywhere on it steps it forward.
    // That second part is the tremor-friendly half: the whole row is one wide
    // target, so nobody has to land on a chevron to change anything.
    if (hoverRow >= 0) {
        m_focus = hoverRow;
        if (!onChevron && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            step = +1; stepWraps = true;
        }
    }

    // --- Keyboard / pad -----------------------------------------------------
    if (in.up)   m_focus = (m_focus + kRowCount - 1) % kRowCount;
    if (in.down) m_focus = (m_focus + 1) % kRowCount;
    if (in.left)    { step = -1; stepWraps = false; }
    if (in.right)   { step = +1; stepWraps = false; }
    if (in.confirm) { step = +1; stepWraps = true;  }
    if (step != 0) {
        const Row& r = kRows[m_focus];
        const int  n = optionCount(r.opts);
        int v = std::clamp(s.*(r.field), 0, n - 1) + step;
        if (v >= n) v = stepWraps ? 0 : n - 1;
        if (v < 0)  v = 0;
        if (v != s.*(r.field)) { s.*(r.field) = v; if (changed) *changed = true; }
    }
    if (changed && *changed) refreshPresetLabel(s);

    // --- Info column --------------------------------------------------------
    {
        const float px0 = ix + rowsW + colGap;
        const float px1 = ix + iw;
        panel(g, px0, rowsTop, px1, rowsBot, A);

        const Row& r = kRows[std::clamp(m_focus, 0, kRowCount - 1)];
        float ty = rowsTop + 20.0f * g.S;
        g.spaced(px0 + 18.0f * g.S, ty, g.fTiny, accent(A * 0.9f), "WHAT IT DOES");
        ty += g.fTiny * 2.0f;
        ty = paragraph(g, px0 + 18.0f * g.S, ty, g.fSmall, fade(kText, A * 0.92f),
                       r.help);
        ty += g.fSmall * 0.9f;
        g.spaced(px0 + 18.0f * g.S, ty, g.fTiny, accent(A * 0.9f), "WHAT IT COSTS");
        ty += g.fTiny * 2.0f;
        paragraph(g, px0 + 18.0f * g.S, ty, g.fSmall, fade(kDim, A), r.cost);

        // --- Live frame rate ------------------------------------------------
        // The whole point of putting this here: a setting is judged by what the
        // frame does when it moves, and that is unreadable if the number lives on
        // a different screen.
        const float gh = 86.0f * g.S;
        const float gy1 = rowsBot - 18.0f * g.S;
        const float gy0 = gy1 - gh;
        const float gx0 = px0 + 18.0f * g.S, gx1 = px1 - 18.0f * g.S;
        dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(gx1, gy1),
                          fade(IM_COL32(255, 255, 255, 10), A), 5.0f * g.S);
        // 60 fps line, so the trace is read against something rather than against
        // its own scale.
        const float ref = gy1 - gh * clamp01(60.0f / 120.0f);
        dl->AddLine(ImVec2(gx0, ref), ImVec2(gx1, ref),
                    fade(IM_COL32(255, 255, 255, 40), A), 1.0f);
        if (m_frames.size() > 1) {
            const float sx = (gx1 - gx0) / static_cast<float>(m_frames.size() - 1);
            for (std::size_t k = 1; k < m_frames.size(); ++k) {
                const float f0 = clamp01(1.0f / std::max(m_frames[k - 1], 1e-4f) / 120.0f);
                const float f1 = clamp01(1.0f / std::max(m_frames[k], 1e-4f) / 120.0f);
                dl->AddLine(ImVec2(gx0 + (k - 1) * sx, gy1 - gh * f0),
                            ImVec2(gx0 + k * sx, gy1 - gh * f1), accent(0.75f * A),
                            1.6f * g.S);
            }
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f", m_fpsShown);
        g.text(gx0 + 6.0f * g.S, gy0 - g.fBig * 1.05f, g.fBig, fade(kText, A), buf);
        const float nw = g.textW(g.fBig, buf);
        g.text(gx0 + 12.0f * g.S + nw, gy0 - g.fSmall * 1.4f, g.fSmall,
               fade(kDim, A), "fps");
        std::snprintf(buf, sizeof(buf), "%.1f ms",
                      1000.0f / std::max(m_fpsShown, 1.0f));
        g.textR(gx1, gy0 - g.fSmall * 1.4f, g.fSmall, fade(kDim, A), buf);
    }

    // --- Bottom bar ---------------------------------------------------------
    {
        const float byc = by - botH * 0.5f;
        dl->AddLine(ImVec2(ix, by - botH), ImVec2(ix + iw, by - botH),
                    fade(kEdge, A), 1.0f);
        g.text(ix, byc - g.fSmall * 0.6f, g.fSmall, fade(kDim, A * 0.85f),
               "Up / Down  choose      Left / Right  change      Enter  step      Esc  back");
        const float bw = 190.0f * g.S, bh = 46.0f * g.S;
        if (bigButton(g, ix + iw - bw, byc - bh * 0.5f, ix + iw, byc + bh * 0.5f,
                      "Back", false, A, pulse))
            setOpen(false);
    }

    if (in.back) setOpen(false);
    return m_open;
}

} // namespace gfxmenu
