#include "WindowIcons.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include <imgui_internal.h>   // the hook, MenuItemEx and the menu's columns

#include "ToolbarIcons.hpp"

namespace winicon {

namespace {

constexpr float kPi = 3.14159265f;

enum class Pic {
    None,
    // General
    View3D, Hierarchy, Inspector,
    // World
    Globe, Terrain, Sculpt, Paint, Water, Rivers, Sky, Weather, PostFx,
    Shadows, Environment, Leaf,
    // Planting
    Fir, Scatter, Tree,
    // Construct
    Road, Splines, City, Town, Level, Building, House, Stairs, Sign, Car,
    Glider, Voxels,
    // 3D Print
    Qr, Card, Gridfinity, Cables, Pens, TerrainModel,
    // Objects
    Modeling, MeshPaint, Uv, Nodes, Cursor,
    // Assets
    Material, Teapot, Retarget, HoldPose, Image, ImagePrompt, Wave, Prefab,
    Folder, Lua, CsScripts, CsEditor, Assistant, Gear, Synth, Notes, Film,
    Import,
    // Presentation
    Hud, Camera, CamPath, Timeline, Sequencer, StateGraph, Render, Strahl,
    Mixer, Recorder,
    // Inspect
    Gauge, Stats, Log,
    // Help
    Book, About,
};

// Every name a window goes by: its Begin title (the part shown, or the ID after
// "###" for the titles that carry a file name), and the labels the View menu and
// the areas' editor menu list it under where those differ. Compared without case.
struct Named { const char* name; Pic pic; };
constexpr Named kNames[] = {
    {"Scene", Pic::View3D}, {"Scene 2", Pic::View3D}, {"Scene 3", Pic::View3D},
    {"Scene 4", Pic::View3D}, {"3D viewport", Pic::View3D}, {"3D viewport 2", Pic::View3D},
    {"3D viewport 3", Pic::View3D}, {"3D viewport 4", Pic::View3D},
    {"Hierarchy", Pic::Hierarchy}, {"Inspector", Pic::Inspector},

    {"World generator", Pic::Globe}, {"Terrain", Pic::Terrain},
    {"Terrain Sculpt", Pic::Sculpt}, {"Terrain Paint", Pic::Paint},
    {"Water", Pic::Water}, {"Rivers", Pic::Rivers}, {"Rivers & brooks", Pic::Rivers},
    {"Sky & atmosphere", Pic::Sky}, {"Weather & audio", Pic::Weather},
    {"Post processing", Pic::PostFx}, {"Shadows & reflections", Pic::Shadows},
    {"Environment", Pic::Environment}, {"Advanced nature", Pic::Leaf},

    {"Vegetation", Pic::Fir}, {"Scatter", Pic::Scatter}, {"Tree generator", Pic::Tree},

    {"Roads", Pic::Road}, {"Splines", Pic::Splines}, {"Splines & bridges", Pic::Splines},
    {"City", Pic::City}, {"Town generator", Pic::Town}, {"Level generator", Pic::Level},
    {"Buildings", Pic::Building}, {"Houses", Pic::House}, {"Stairs", Pic::Stairs},
    {"Street signs", Pic::Sign}, {"Vehicle", Pic::Car}, {"Glider", Pic::Glider},
    {"Voxels", Pic::Voxels},

    {"QR codes", Pic::Qr}, {"Cards", Pic::Card}, {"Gridfinity", Pic::Gridfinity},
    {"Cable holders", Pic::Cables}, {"Pen holder", Pic::Pens},
    {"Terrain model", Pic::TerrainModel},

    {"Modeling", Pic::Modeling}, {"Mesh Paint", Pic::MeshPaint}, {"UV editor", Pic::Uv},
    {"Procedural", Pic::Nodes}, {"3D Cursor", Pic::Cursor},

    {"Materials", Pic::Material}, {"Models", Pic::Teapot},
    {"Retarget animations", Pic::Retarget}, {"Hold Pose Studio", Pic::HoldPose},
    {"Image editor", Pic::Image}, {"New image from prompt", Pic::ImagePrompt},
    {"Wave editor", Pic::Wave}, {"Prefabs", Pic::Prefab}, {"Assets", Pic::Folder},
    {"Scripts", Pic::Lua}, {"C# Scripts", Pic::CsScripts}, {"C# Editor", Pic::CsEditor},
    {"Assistant", Pic::Assistant}, {"Assistant (AI)", Pic::Assistant},
    {"AI settings", Pic::Gear}, {"AI settings...", Pic::Gear},
    {"Synth", Pic::Synth}, {"Song Sequencer", Pic::Notes}, {"Video Sequencer", Pic::Film},
    {"Import Unity asset", Pic::Import},

    {"UI Overlay", Pic::Hud}, {"Camera", Pic::Camera}, {"Camera path", Pic::CamPath},
    {"Timeline", Pic::Timeline}, {"Sequencer", Pic::Sequencer},
    {"Animation graph", Pic::StateGraph}, {"Render", Pic::Render},
    {"Strahl", Pic::Strahl}, {"Strahl (scene script)", Pic::Strahl},
    {"Mixer", Pic::Mixer}, {"Recorder", Pic::Recorder},

    {"Performance", Pic::Gauge}, {"Stats", Pic::Stats}, {"Log", Pic::Log},

    {"Lua API", Pic::Book}, {"Strahl Reference", Pic::Book},
    {"About Fitzel", Pic::About}, {"About Fitzel...", Pic::About},
};

bool sameText(const char* a, std::size_t n, const char* b) {
    for (std::size_t i = 0; i < n; ++i, ++b) {
        if (!*b) return false;
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] - 'A' + 'a') : a[i];
        const char y = *b   >= 'A' && *b   <= 'Z' ? char(*b - 'A' + 'a')   : *b;
        if (x != y) return false;
    }
    return *b == '\0';
}

Pic lookup(const char* s, std::size_t n) {
    for (const Named& e : kNames)
        if (sameText(s, n, e.name)) return e.pic;
    return Pic::None;
}

// What is shown first (up to "##"), then the ID after "###": "Song Sequencer -
// demo *###Song Sequencer" is the Song Sequencer by the second.
Pic find(const char* name) {
    if (!name || !*name) return Pic::None;
    const char* hash = std::strstr(name, "##");
    const Pic shown = lookup(name, hash ? std::size_t(hash - name) : std::strlen(name));
    if (shown != Pic::None) return shown;
    if (const char* id = std::strstr(name, "###")) return lookup(id + 3, std::strlen(id + 3));
    return Pic::None;
}

// --- The pictures -------------------------------------------------------------
// Line pictures in a -1..1 square, y down, like the toolbar's: what the window
// makes or shows, not what it looks like. The ones the toolbar strip already has
// come from there, so a window reads the same on its button and on its tab.
void paint(ImDrawList* dl, Pic pic, ImVec2 c, float r, ImU32 col) {
    using icon::Tool;
    const float lw = std::max(1.0f, r * 0.17f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    auto line = [&](float x0, float y0, float x1, float y1, float k = 1.0f) {
        dl->AddLine(P(x0, y0), P(x1, y1), col, lw * k);
    };
    auto rect = [&](float x0, float y0, float x1, float y1, float round = 0.0f, float k = 1.0f) {
        dl->AddRect(P(x0, y0), P(x1, y1), col, round * r, 0, lw * k);
    };
    auto fill = [&](float x0, float y0, float x1, float y1, float round = 0.0f) {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), col, round * r);
    };
    auto ring = [&](float x, float y, float rad, float k = 1.0f) {
        dl->AddCircle(P(x, y), rad * r, col, 0, lw * k);
    };
    auto dot = [&](float x, float y, float rad) { dl->AddCircleFilled(P(x, y), rad * r, col); };
    auto tri = [&](float x0, float y0, float x1, float y1, float x2, float y2) {
        dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), col);
    };
    auto poly = [&](std::initializer_list<ImVec2> pts, bool closed, float k = 1.0f) {
        ImVec2 q[16];
        int n = 0;
        for (const ImVec2& p : pts) if (n < 16) q[n++] = P(p.x, p.y);
        dl->AddPolyline(q, n, col, closed ? ImDrawFlags_Closed : 0, lw * k);
    };
    // An arrow from (x0, y0) to (x1, y1), its head at the end.
    auto arrow = [&](float x0, float y0, float x1, float y1, float head = 0.38f) {
        const float dx = x1 - x0, dy = y1 - y0, l = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
        const float ux = dx / l, uy = dy / l;
        line(x0, y0, x1 - ux * head * 0.6f, y1 - uy * head * 0.6f);
        tri(x1, y1, x1 - ux * head - uy * head * 0.6f, y1 - uy * head + ux * head * 0.6f,
            x1 - ux * head + uy * head * 0.6f, y1 - uy * head - ux * head * 0.6f);
    };
    // A four-pointed star: two thin diamonds crossed (the draw list only fills
    // convex shapes), the toolbar's shorthand for "effects".
    auto sparkle = [&](float x, float y, float a) {
        const float w = a * 0.3f;
        dl->AddQuadFilled(P(x, y - a), P(x + w, y), P(x, y + a), P(x - w, y), col);
        dl->AddQuadFilled(P(x - a, y), P(x, y - w), P(x + a, y), P(x, y + w), col);
    };
    // A stick figure standing on y = foot, `h` tall, centred on x.
    auto figure = [&](float x, float foot, float h) {
        ring(x, foot - h * 0.86f, h * 0.14f, 0.9f);
        line(x, foot - h * 0.72f, x, foot - h * 0.35f);
        line(x, foot - h * 0.35f, x - h * 0.2f, foot);
        line(x, foot - h * 0.35f, x + h * 0.2f, foot);
        line(x - h * 0.25f, foot - h * 0.6f, x + h * 0.25f, foot - h * 0.6f);
    };
    // A page with its corner turned down.
    auto page = [&](float x0, float y0, float x1, float y1) {
        const float k = (x1 - x0) * 0.32f;
        poly({{x0, y0}, {x1 - k, y0}, {x1, y0 + k}, {x1, y1}, {x0, y1}}, true);
        poly({{x1 - k, y0}, {x1 - k, y0 + k}, {x1, y0 + k}}, false, 0.8f);
    };
    // A '#': two strokes leaning right, two across.
    auto hash = [&](float x, float y, float s) {
        line(x - s * 0.45f, y + s, x - s * 0.15f, y - s, 0.85f);
        line(x + s * 0.15f, y + s, x + s * 0.45f, y - s, 0.85f);
        line(x - s * 0.8f, y - s * 0.35f, x + s * 0.9f, y - s * 0.35f, 0.85f);
        line(x - s * 0.9f, y + s * 0.35f, x + s * 0.8f, y + s * 0.35f, 0.85f);
    };
    auto house = [&](float x0, float x1, float base, float eave, float peak) {
        poly({{x0, base}, {x0, eave}, {0.5f * (x0 + x1), peak}, {x1, eave}, {x1, base}}, true);
    };

    switch (pic) {
    case Pic::None:
        break;

    // --- General
    case Pic::View3D: // a box in perspective: the 3D view
        rect(-0.9f, -0.35f, 0.35f, 0.9f);
        poly({{-0.9f, -0.35f}, {-0.35f, -0.9f}, {0.9f, -0.9f}, {0.9f, 0.35f}, {0.35f, 0.9f}}, false);
        line(0.35f, -0.35f, 0.9f, -0.9f);
        break;
    case Pic::Hierarchy: // a parent and its two children
        fill(-0.95f, -0.95f, -0.45f, -0.45f);
        line(-0.25f, -0.7f, 0.95f, -0.7f);
        line(-0.7f, -0.45f, -0.7f, 0.7f);
        line(-0.7f, 0.05f, -0.3f, 0.05f);
        line(-0.7f, 0.7f, -0.3f, 0.7f);
        fill(-0.3f, -0.17f, 0.15f, 0.28f);
        fill(-0.3f, 0.48f, 0.15f, 0.93f);
        line(0.35f, 0.05f, 0.95f, 0.05f);
        line(0.35f, 0.7f, 0.95f, 0.7f);
        break;
    case Pic::Inspector: // the properties: three sliders
        for (int i = 0; i < 3; ++i) {
            const float y = -0.65f + i * 0.65f;
            const float k = i == 0 ? -0.3f : i == 1 ? 0.45f : -0.6f;
            line(-0.95f, y, 0.95f, y, 0.8f);
            dot(k, y, 0.24f);
        }
        break;

    // --- World
    case Pic::Globe: // the world: meridians and parallels
        ring(0.0f, 0.0f, 0.92f);
        dl->AddEllipse(c, ImVec2(r * 0.4f, r * 0.92f), col, 0.0f, 0, lw * 0.85f);
        line(-0.92f, 0.0f, 0.92f, 0.0f, 0.85f);
        line(-0.75f, -0.5f, 0.75f, -0.5f, 0.7f);
        line(-0.75f, 0.5f, 0.75f, 0.5f, 0.7f);
        break;
    case Pic::Terrain: // two peaks, the higher one capped with snow
        poly({{-1.0f, 0.75f}, {-0.45f, -0.2f}, {-0.1f, 0.3f}, {0.4f, -0.75f}, {1.0f, 0.75f}}, true);
        tri(0.4f, -0.75f, 0.16f, -0.25f, 0.62f, -0.28f);
        break;
    case Pic::Sculpt:      icon::tool(dl, Tool::Sculpt, c, r, col); break;
    case Pic::Paint:       icon::tool(dl, Tool::Paint, c, r, col); break;
    case Pic::Water:       icon::tool(dl, Tool::Water, c, r, col); break;
    case Pic::Rivers:      icon::tool(dl, Tool::Rivers, c, r, col); break;
    case Pic::Weather:     icon::tool(dl, Tool::Weather, c, r, col); break;
    case Pic::Environment: icon::tool(dl, Tool::Environment, c, r, col); break;
    case Pic::PostFx:      icon::postFx(dl, c, r, col); break;
    case Pic::Sky: { // the sun over a cloud (the toolbar's, without the cut-out
                     // it needs the strip's background for)
        const ImVec2 s = P(0.3f, -0.4f);
        dl->AddCircle(s, r * 0.4f, col, 0, lw);
        for (int i = 0; i < 5; ++i) {
            const float ang = -2.6f + i * 0.62f;
            const ImVec2 d(std::cos(ang), std::sin(ang));
            dl->AddLine({s.x + d.x * r * 0.58f, s.y + d.y * r * 0.58f},
                        {s.x + d.x * r * 0.85f, s.y + d.y * r * 0.85f}, col, lw * 0.85f);
        }
        dot(-0.5f, 0.42f, 0.32f);
        dot(-0.05f, 0.25f, 0.4f);
        dot(0.38f, 0.45f, 0.28f);
        fill(-0.82f, 0.45f, 0.66f, 0.75f, 0.15f);
        break;
    }
    case Pic::Shadows: // a ball and the shadow it casts
        dl->AddEllipseFilled(P(0.3f, 0.72f), ImVec2(r * 0.68f, r * 0.2f), col);
        ring(-0.15f, -0.2f, 0.6f);
        dot(-0.38f, -0.42f, 0.13f);
        break;
    case Pic::Leaf: // a leaf and its veins
        dl->AddBezierQuadratic(P(-0.85f, 0.85f), P(-0.9f, -0.85f), P(0.9f, -0.9f), col, lw);
        dl->AddBezierQuadratic(P(-0.85f, 0.85f), P(0.85f, 0.9f), P(0.9f, -0.9f), col, lw);
        line(-0.85f, 0.85f, 0.45f, -0.45f, 0.85f);
        line(-0.1f, 0.1f, -0.1f, -0.35f, 0.7f);
        line(-0.1f, 0.1f, 0.35f, 0.1f, 0.7f);
        break;

    // --- Planting
    case Pic::Fir: icon::tool(dl, Tool::Vegetation, c, r, col); break;
    case Pic::Scatter: // things strewn across the ground
        dot(-0.6f, -0.55f, 0.2f); dot(0.15f, -0.75f, 0.14f); dot(0.7f, -0.35f, 0.22f);
        dot(-0.15f, -0.05f, 0.24f); dot(-0.75f, 0.45f, 0.16f); dot(0.45f, 0.35f, 0.18f);
        dot(-0.05f, 0.75f, 0.2f); dot(0.85f, 0.85f, 0.12f);
        break;
    case Pic::Tree: // a broad-leaved tree: a crown on a forked trunk
        dot(-0.38f, -0.15f, 0.42f);
        dot(0.38f, -0.2f, 0.42f);
        dot(0.0f, -0.55f, 0.45f);
        fill(-0.12f, 0.1f, 0.12f, 0.95f);
        line(0.0f, 0.45f, -0.4f, 0.1f, 0.9f);
        line(0.0f, 0.35f, 0.4f, 0.05f, 0.9f);
        break;

    // --- Construct
    case Pic::Road:    icon::tool(dl, Tool::Road, c, r, col); break;
    case Pic::Splines: icon::tool(dl, Tool::Splines, c, r, col); break;
    case Pic::City: // a skyline
        poly({{-1.0f, 0.9f}, {-1.0f, -0.2f}, {-0.45f, -0.2f}, {-0.45f, -0.85f}, {0.15f, -0.85f},
              {0.15f, -0.45f}, {0.55f, -0.45f}, {0.55f, 0.1f}, {1.0f, 0.1f}, {1.0f, 0.9f}}, true);
        for (float y : {-0.6f, -0.3f, 0.0f, 0.3f}) fill(-0.27f, y, -0.05f, y + 0.14f);
        for (float y : {0.15f, 0.45f}) fill(-0.8f, y, -0.6f, y + 0.14f);
        for (float y : {-0.2f, 0.15f, 0.45f}) fill(0.27f, y, 0.42f, y + 0.14f);
        break;
    case Pic::Town: // two houses, one behind the other
        poly({{-0.15f, 0.0f}, {-0.15f, -0.25f}, {-0.55f, -0.95f}, {-0.95f, -0.25f}, {-0.95f, 0.9f},
              {-0.3f, 0.9f}}, false);
        house(-0.3f, 0.95f, 0.9f, 0.05f, -0.55f);
        fill(0.2f, 0.4f, 0.45f, 0.9f);
        break;
    case Pic::Level: // a floor plan: rooms and the doors between them
        rect(-0.95f, -0.95f, 0.95f, 0.95f);
        line(-0.1f, -0.95f, -0.1f, -0.35f);
        line(-0.1f, 0.05f, -0.1f, 0.95f);
        line(-0.95f, 0.2f, -0.55f, 0.2f);
        line(-0.25f, 0.2f, -0.1f, 0.2f);
        line(-0.1f, -0.1f, 0.4f, -0.1f);
        line(0.7f, -0.1f, 0.95f, -0.1f);
        break;
    case Pic::Building: // a tower block and its windows
        rect(-0.6f, -0.95f, 0.6f, 0.95f);
        for (int row = 0; row < 4; ++row)
            for (int col2 = 0; col2 < 2; ++col2) {
                const float x = -0.38f + col2 * 0.5f, y = -0.72f + row * 0.4f;
                fill(x, y, x + 0.26f, y + 0.2f);
            }
        break;
    case Pic::House: // a house with its door
        house(-0.85f, 0.85f, 0.9f, -0.1f, -0.9f);
        rect(-0.2f, 0.3f, 0.2f, 0.9f, 0.0f, 0.85f);
        break;
    case Pic::Stairs: // a flight of steps
        poly({{-0.95f, 0.9f}, {-0.95f, 0.45f}, {-0.45f, 0.45f}, {-0.45f, 0.0f}, {0.05f, 0.0f},
              {0.05f, -0.45f}, {0.5f, -0.45f}, {0.5f, -0.9f}, {0.95f, -0.9f}, {0.95f, 0.9f}}, true);
        break;
    case Pic::Sign: // a signpost pointing on
        poly({{-0.85f, -0.85f}, {0.55f, -0.85f}, {0.95f, -0.5f}, {0.55f, -0.15f}, {-0.85f, -0.15f}}, true);
        line(-0.45f, -0.15f, -0.45f, 0.95f, 1.2f);
        line(-0.8f, 0.95f, -0.1f, 0.95f);
        break;
    case Pic::Car: // a car from the side
        poly({{-0.95f, 0.45f}, {-0.95f, 0.0f}, {-0.55f, -0.1f}, {-0.25f, -0.55f}, {0.35f, -0.55f},
              {0.65f, -0.05f}, {0.95f, 0.05f}, {0.95f, 0.45f}}, true);
        line(-0.1f, -0.55f, -0.1f, -0.08f, 0.8f);
        ring(-0.5f, 0.5f, 0.24f);
        ring(0.5f, 0.5f, 0.24f);
        break;
    case Pic::Glider: { // a glider from above: long wings, a slim body, the tail
        const ImVec2 wing[4] = {P(-1.0f, -0.25f), P(1.0f, -0.25f), P(0.95f, -0.05f), P(-0.95f, -0.05f)};
        dl->AddConvexPolyFilled(wing, 4, col);
        line(0.0f, -0.65f, 0.0f, 0.8f, 1.1f);
        line(-0.35f, 0.75f, 0.35f, 0.75f, 1.1f);
        break;
    }
    case Pic::Voxels: // three blocks, stacked
        rect(-0.9f, 0.0f, -0.05f, 0.85f);
        rect(0.05f, 0.0f, 0.9f, 0.85f);
        rect(-0.42f, -0.9f, 0.42f, -0.1f);
        break;

    // --- 3D Print
    case Pic::Qr: { // the three finder squares and a few modules
        auto finder = [&](float x, float y) {
            rect(x, y, x + 0.75f, y + 0.75f, 0.0f, 0.85f);
            fill(x + 0.23f, y + 0.23f, x + 0.52f, y + 0.52f);
        };
        finder(-0.95f, -0.95f);
        finder(0.2f, -0.95f);
        finder(-0.95f, 0.2f);
        fill(0.2f, 0.2f, 0.45f, 0.45f);
        fill(0.7f, 0.45f, 0.95f, 0.7f);
        fill(0.45f, 0.7f, 0.7f, 0.95f);
        break;
    }
    case Pic::Card: // a card: its heading and two lines
        rect(-0.95f, -0.7f, 0.95f, 0.7f, 0.15f);
        line(-0.6f, -0.3f, 0.3f, -0.3f, 1.3f);
        line(-0.6f, 0.1f, 0.6f, 0.1f, 0.75f);
        line(-0.6f, 0.38f, 0.35f, 0.38f, 0.75f);
        break;
    case Pic::Gridfinity: // four bins on their baseplate
        rect(-0.97f, -0.97f, 0.97f, 0.97f, 0.15f, 0.8f);
        for (float x : {-0.72f, 0.08f})
            for (float y : {-0.72f, 0.08f})
                rect(x, y, x + 0.64f, y + 0.64f, 0.12f);
        break;
    case Pic::Cables: // a comb, a cable between each of its teeth
        rect(-0.95f, 0.45f, 0.95f, 0.85f, 0.1f);
        for (float x : {-0.85f, -0.28f, 0.28f, 0.85f}) line(x, 0.45f, x, -0.55f, 1.1f);
        for (float x : {-0.565f, 0.0f, 0.565f}) ring(x, 0.05f, 0.17f, 0.9f);
        break;
    case Pic::Pens: // a honeycomb cup, two pens in it
        line(-0.35f, 0.1f, -0.75f, -0.9f, 1.1f);
        tri(-0.75f, -0.9f, -0.82f, -0.62f, -0.6f, -0.7f);
        line(0.25f, 0.1f, 0.55f, -0.95f, 1.1f);
        rect(-0.7f, -0.05f, 0.7f, 0.95f, 0.08f);
        line(-0.23f, -0.05f, -0.23f, 0.95f, 0.75f);
        line(0.23f, -0.05f, 0.23f, 0.95f, 0.75f);
        line(-0.7f, 0.45f, 0.7f, 0.45f, 0.75f);
        break;
    case Pic::TerrainModel: // a relief in terraces, on its slab
        rect(-0.95f, 0.5f, 0.95f, 0.88f);
        poly({{-0.95f, 0.5f}, {-0.95f, 0.18f}, {-0.55f, 0.18f}, {-0.55f, -0.18f}, {-0.15f, -0.18f},
              {-0.15f, -0.6f}, {0.3f, -0.6f}, {0.3f, -0.15f}, {0.65f, -0.15f}, {0.65f, 0.18f},
              {0.95f, 0.18f}, {0.95f, 0.5f}}, false);
        break;

    // --- Objects
    case Pic::Modeling: icon::tool(dl, Tool::Modeling, c, r, col); break;
    case Pic::MeshPaint: // a brush on a ball
        ring(-0.3f, 0.3f, 0.62f);
        line(0.95f, -0.95f, 0.35f, -0.35f, 1.3f);
        tri(0.5f, -0.2f, 0.2f, -0.5f, -0.05f, 0.05f);
        break;
    case Pic::Uv: { // the checker a UV map is tried on with
        rect(-0.92f, -0.92f, 0.92f, 0.92f, 0.0f, 0.8f);
        const float k = 1.84f / 4.0f;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if ((i + j) % 2 == 0)
                    fill(-0.92f + i * k, -0.92f + j * k, -0.92f + (i + 1) * k, -0.92f + (j + 1) * k);
        break;
    }
    case Pic::Nodes: // two nodes wired into a third
        rect(-0.95f, -0.85f, -0.35f, -0.4f, 0.1f);
        rect(-0.95f, 0.4f, -0.35f, 0.85f, 0.1f);
        rect(0.35f, -0.25f, 0.95f, 0.25f, 0.1f);
        dl->AddBezierCubic(P(-0.35f, -0.62f), P(0.0f, -0.62f), P(0.0f, 0.0f), P(0.35f, 0.0f), col, lw * 0.85f);
        dl->AddBezierCubic(P(-0.35f, 0.62f), P(0.0f, 0.62f), P(0.0f, 0.0f), P(0.35f, 0.0f), col, lw * 0.85f);
        break;
    case Pic::Cursor: { // the 3D cursor: a ring in red and white, cross-haired
        const ImU32 red = IM_COL32(230, 70, 60, 255);
        for (int i = 0; i < 8; ++i) {
            const float a0 = i * kPi * 0.25f;
            dl->PathArcTo(c, r * 0.62f, a0, a0 + kPi * 0.25f, 4);
            dl->PathStroke(i % 2 ? col : red, 0, lw * 1.1f);
        }
        line(-1.0f, 0.0f, -0.35f, 0.0f, 0.8f);
        line(0.35f, 0.0f, 1.0f, 0.0f, 0.8f);
        line(0.0f, -1.0f, 0.0f, -0.35f, 0.8f);
        line(0.0f, 0.35f, 0.0f, 1.0f, 0.8f);
        break;
    }

    // --- Assets
    case Pic::Material: icon::tool(dl, Tool::Materials, c, r, col); break;
    case Pic::Prefab:   icon::tool(dl, Tool::Prefabs, c, r, col); break;
    case Pic::Folder:   icon::tool(dl, Tool::Assets, c, r, col); break;
    case Pic::Teapot: // the teapot every 3D package starts from: a model
        dl->AddEllipse(P(-0.05f, 0.3f), ImVec2(r * 0.62f, r * 0.5f), col, 0.0f, 0, lw);
        line(-0.4f, -0.2f, 0.3f, -0.2f);
        dot(-0.05f, -0.38f, 0.13f);
        dl->AddBezierQuadratic(P(0.5f, 0.35f), P(0.75f, 0.2f), P(0.98f, -0.25f), col, lw);
        dl->AddBezierCubic(P(-0.58f, 0.05f), P(-1.05f, -0.05f), P(-1.05f, 0.55f), P(-0.6f, 0.5f), col, lw);
        break;
    case Pic::Retarget: // one figure's motion handed to another
        figure(-0.55f, 0.95f, 1.35f);
        figure(0.55f, 0.95f, 1.35f);
        arrow(-0.35f, -0.85f, 0.4f, -0.85f, 0.3f);
        break;
    case Pic::HoldPose: // a figure holding something out
        figure(-0.35f, 0.95f, 1.8f);
        line(-0.35f, -0.13f, 0.35f, -0.35f);
        fill(0.35f, -0.65f, 0.85f, -0.15f);
        break;
    case Pic::Image: // a picture: a hill under the sun, in its frame
        rect(-0.95f, -0.8f, 0.95f, 0.8f, 0.08f);
        poly({{-0.75f, 0.6f}, {-0.2f, -0.1f}, {0.15f, 0.3f}, {0.4f, 0.05f}, {0.75f, 0.6f}}, true, 0.85f);
        dot(0.45f, -0.42f, 0.16f);
        break;
    case Pic::ImagePrompt: // a picture conjured up
        rect(-0.95f, -0.45f, 0.5f, 0.85f, 0.08f);
        poly({{-0.8f, 0.7f}, {-0.35f, 0.15f}, {-0.05f, 0.45f}, {0.15f, 0.3f}, {0.35f, 0.7f}}, true, 0.8f);
        sparkle(0.62f, -0.55f, 0.42f);
        break;
    case Pic::Wave: // a waveform
        for (int i = 0; i < 9; ++i) {
            static const float kH[9] = {0.2f, 0.5f, 0.85f, 0.4f, 0.95f, 0.6f, 0.3f, 0.7f, 0.25f};
            const float x = -0.88f + i * 0.22f;
            line(x, -kH[i], x, kH[i], 0.95f);
        }
        break;
    case Pic::Lua: // code: a pair of angle brackets and a slash
        poly({{-0.4f, -0.6f}, {-0.95f, 0.0f}, {-0.4f, 0.6f}}, false, 1.1f);
        poly({{0.4f, -0.6f}, {0.95f, 0.0f}, {0.4f, 0.6f}}, false, 1.1f);
        line(0.15f, -0.85f, -0.15f, 0.85f);
        break;
    case Pic::CsScripts: // C# files: two pages, a '#' on the front one
        poly({{-0.75f, 0.55f}, {-0.75f, -0.95f}, {0.45f, -0.95f}}, false, 0.8f);
        page(-0.5f, -0.7f, 0.9f, 0.95f);
        hash(0.18f, 0.2f, 0.38f);
        break;
    case Pic::CsEditor: // a window with C# in it
        rect(-0.95f, -0.85f, 0.95f, 0.85f, 0.08f);
        line(-0.95f, -0.45f, 0.95f, -0.45f, 0.8f);
        hash(-0.3f, 0.22f, 0.4f);
        line(0.3f, 0.5f, 0.75f, 0.5f, 0.9f);
        break;
    case Pic::Assistant: // a speech bubble, sparkling: the AI assistant
        dl->AddEllipse(P(0.0f, -0.12f), ImVec2(r * 0.95f, r * 0.72f), col, 0.0f, 0, lw);
        tri(-0.55f, 0.35f, -0.15f, 0.55f, -0.75f, 0.95f);
        sparkle(0.0f, -0.12f, 0.45f);
        break;
    case Pic::Gear: { // settings: a cog
        const int teeth = 8;
        for (int i = 0; i < teeth; ++i) {
            const float a = i * 2.0f * kPi / teeth;
            const ImVec2 d(std::cos(a), std::sin(a)), t(-d.y, d.x);
            const float w = 0.16f;
            const ImVec2 q[4] = {
                P(d.x * 0.55f + t.x * w, d.y * 0.55f + t.y * w), P(d.x * 0.95f + t.x * w, d.y * 0.95f + t.y * w),
                P(d.x * 0.95f - t.x * w, d.y * 0.95f - t.y * w), P(d.x * 0.55f - t.x * w, d.y * 0.55f - t.y * w)};
            dl->AddConvexPolyFilled(q, 4, col);
        }
        ring(0.0f, 0.0f, 0.6f, 1.4f);
        ring(0.0f, 0.0f, 0.2f);
        break;
    }
    case Pic::Synth: // a keyboard: white keys and the black ones between
        rect(-0.95f, -0.8f, 0.95f, 0.8f, 0.05f);
        for (float x : {-0.475f, 0.0f, 0.475f}) line(x, -0.8f, x, 0.8f, 0.75f);
        for (float x : {-0.475f, 0.475f}) fill(x - 0.15f, -0.8f, x + 0.15f, 0.15f);
        break;
    case Pic::Notes: // two quavers under one beam
        dl->AddEllipseFilled(P(-0.55f, 0.65f), ImVec2(r * 0.3f, r * 0.22f), col, -0.4f);
        dl->AddEllipseFilled(P(0.55f, 0.45f), ImVec2(r * 0.3f, r * 0.22f), col, -0.4f);
        line(-0.3f, 0.6f, -0.3f, -0.6f);
        line(0.8f, 0.4f, 0.8f, -0.85f);
        {
            const ImVec2 beam[4] = {P(-0.3f - lw / r * 0.5f, -0.6f), P(0.8f + lw / r * 0.5f, -0.85f),
                                    P(0.8f + lw / r * 0.5f, -0.55f), P(-0.3f - lw / r * 0.5f, -0.3f)};
            dl->AddConvexPolyFilled(beam, 4, col);
        }
        break;
    case Pic::Film: // a strip of film
        rect(-0.95f, -0.8f, 0.95f, 0.8f, 0.05f);
        line(-0.95f, -0.42f, 0.95f, -0.42f, 0.75f);
        line(-0.95f, 0.42f, 0.95f, 0.42f, 0.75f);
        for (float x : {-0.68f, -0.23f, 0.23f, 0.68f}) {
            fill(x - 0.1f, -0.68f, x + 0.1f, -0.55f);
            fill(x - 0.1f, 0.55f, x + 0.1f, 0.68f);
        }
        line(0.0f, -0.42f, 0.0f, 0.42f, 0.75f);
        break;
    case Pic::Import: // into the project: an arrow down into a tray
        arrow(0.0f, -0.95f, 0.0f, 0.35f, 0.5f);
        poly({{-0.95f, 0.1f}, {-0.95f, 0.85f}, {0.95f, 0.85f}, {0.95f, 0.1f}}, false);
        break;

    // --- Presentation
    case Pic::Hud: // a screen with a game's overlay on it: a bar, a map
        rect(-0.95f, -0.75f, 0.95f, 0.75f, 0.08f);
        fill(-0.75f, -0.55f, -0.05f, -0.35f);
        ring(0.55f, 0.32f, 0.25f, 0.85f);
        break;
    case Pic::Camera: icon::camera(dl, c, r, col); break;
    case Pic::CamPath: { // a camera and the path it flies
        icon::camera(dl, P(0.35f, -0.35f), r * 0.55f, col);
        ImVec2 q[3] = {P(-0.95f, 0.9f), P(-0.6f, 0.05f), P(0.25f, 0.4f)};
        for (const ImVec2& p : q) dl->AddCircleFilled(p, std::max(1.0f, r * 0.12f), col);
        dl->AddLine(q[0], q[1], col, lw * 0.6f);
        dl->AddLine(q[1], q[2], col, lw * 0.6f);
        break;
    }
    case Pic::Timeline: { // keys on a track, and the playhead
        line(-0.95f, 0.2f, 0.95f, 0.2f, 0.8f);
        for (float x : {-0.65f, 0.05f, 0.6f}) {
            const ImVec2 k[4] = {P(x, -0.05f), P(x + 0.22f, 0.2f), P(x, 0.45f), P(x - 0.22f, 0.2f)};
            dl->AddConvexPolyFilled(k, 4, col);
        }
        line(-0.3f, -0.95f, -0.3f, 0.95f, 0.9f);
        tri(-0.52f, -0.95f, -0.08f, -0.95f, -0.3f, -0.65f);
        break;
    }
    case Pic::Sequencer: // clips on their tracks
        fill(-0.95f, -0.85f, 0.15f, -0.4f, 0.1f);
        fill(-0.5f, -0.22f, 0.95f, 0.22f, 0.1f);
        fill(-0.95f, 0.4f, -0.2f, 0.85f, 0.1f);
        fill(0.05f, 0.4f, 0.7f, 0.85f, 0.1f);
        break;
    case Pic::StateGraph: // states and the transitions between them
        ring(-0.6f, -0.55f, 0.32f);
        ring(0.6f, -0.55f, 0.32f);
        ring(0.0f, 0.6f, 0.32f);
        arrow(-0.22f, -0.55f, 0.22f, -0.55f, 0.28f);
        arrow(0.42f, -0.22f, 0.2f, 0.3f, 0.28f);
        arrow(-0.2f, 0.3f, -0.42f, -0.22f, 0.28f);
        break;
    case Pic::Render: // a ray off a ball, out to the light: the path tracer
        line(-0.95f, 0.9f, 0.95f, 0.9f, 0.85f);
        ring(0.35f, 0.35f, 0.45f);
        line(-0.95f, -0.75f, -0.05f, 0.12f, 0.9f);
        arrow(-0.05f, 0.12f, 0.15f, -0.9f, 0.32f);
        dot(-0.95f, -0.75f, 0.12f);
        break;
    case Pic::Strahl: // a scene script: a page with a ray across it
        page(-0.8f, -0.95f, 0.8f, 0.95f);
        line(-0.5f, 0.55f, 0.05f, -0.05f, 0.9f);
        arrow(0.05f, -0.05f, 0.5f, 0.45f, 0.3f);
        line(-0.5f, -0.45f, 0.1f, -0.45f, 0.75f);
        break;
    case Pic::Mixer: // three faders
        for (int i = 0; i < 3; ++i) {
            const float x = -0.65f + i * 0.65f;
            const float y = i == 0 ? 0.3f : i == 1 ? -0.4f : 0.1f;
            line(x, -0.95f, x, 0.95f, 0.75f);
            fill(x - 0.24f, y - 0.14f, x + 0.24f, y + 0.14f, 0.05f);
        }
        break;
    case Pic::Recorder: // the red light that says "recording"
        ring(0.0f, 0.0f, 0.88f);
        dl->AddCircleFilled(c, r * 0.5f, IM_COL32(230, 70, 60, 255));
        break;

    // --- Inspect
    case Pic::Gauge: { // a speedometer
        const ImVec2 m = P(0.0f, 0.35f);
        dl->PathArcTo(m, r * 0.9f, kPi, 2.0f * kPi, 16);
        dl->PathStroke(col, 0, lw);
        for (int i = 0; i < 5; ++i) {
            const float a = kPi + i * kPi * 0.25f;
            const ImVec2 d(std::cos(a), std::sin(a));
            dl->AddLine({m.x + d.x * r * 0.62f, m.y + d.y * r * 0.62f},
                        {m.x + d.x * r * 0.9f, m.y + d.y * r * 0.9f}, col, lw * 0.75f);
        }
        dl->AddLine(m, {m.x + r * 0.45f, m.y - r * 0.5f}, col, lw * 1.1f);
        dl->AddCircleFilled(m, r * 0.15f, col);
        line(-0.9f, 0.7f, 0.9f, 0.7f, 0.8f);
        break;
    }
    case Pic::Stats: // a bar chart
        line(-0.95f, 0.9f, 0.95f, 0.9f, 0.8f);
        fill(-0.8f, 0.2f, -0.4f, 0.9f);
        fill(-0.2f, -0.45f, 0.2f, 0.9f);
        fill(0.4f, -0.9f, 0.8f, 0.9f);
        break;
    case Pic::Log: // a terminal: the prompt, a line of output
        rect(-0.95f, -0.8f, 0.95f, 0.8f, 0.08f);
        poly({{-0.65f, -0.4f}, {-0.3f, -0.1f}, {-0.65f, 0.2f}}, false);
        line(-0.15f, 0.35f, 0.6f, 0.35f);
        break;

    // --- Help
    case Pic::Book: // an open book
        poly({{0.0f, -0.6f}, {-0.45f, -0.85f}, {-0.95f, -0.75f}, {-0.95f, 0.7f}, {-0.45f, 0.6f}, {0.0f, 0.85f}}, false);
        poly({{0.0f, -0.6f}, {0.45f, -0.85f}, {0.95f, -0.75f}, {0.95f, 0.7f}, {0.45f, 0.6f}, {0.0f, 0.85f}}, false);
        line(0.0f, -0.6f, 0.0f, 0.85f, 0.8f);
        break;
    case Pic::About: // the "i" of "information"
        ring(0.0f, 0.0f, 0.92f);
        dot(0.0f, -0.45f, 0.13f);
        line(0.0f, -0.15f, 0.0f, 0.55f, 1.3f);
        break;
    }
}

// The window's picture, once looked up: the hook asks every frame, for every
// tab and title bar. Keyed by ID, which a title's changing part (a file name,
// "*" for unsaved) leaves alone.
Pic ofWindow(ImGuiWindow* w) {
    static std::unordered_map<ImGuiID, Pic> cache;
    const auto it = cache.find(w->ID);
    if (it != cache.end()) return it->second;
    // By name alone: a docked window carries ChildWindow too, and the children,
    // popups and tooltips that really are such go by names nothing here matches.
    const Pic p = find(w->Name);
    cache.emplace(w->ID, p);
    return p;
}

// A picture fills the line's height, a little inside it so it sits with the
// letters rather than overpowering them.
float radiusFor(float size) { return size * 0.44f; }

float hook(ImDrawList* dl, ImGuiWindow* w, ImVec2 pos, float size) {
    const Pic p = ofWindow(w);
    if (p == Pic::None) return 0.0f;
    if (dl) paint(dl, p, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), radiusFor(size),
                  ImGui::GetColorU32(ImGuiCol_Text));
    return size + ImGui::GetStyle().ItemInnerSpacing.x;
}

// Blanks as wide as a picture and its gap, for the icon column of a menu item
// (MenuItemEx sizes it by the text it is given) and the front of a selectable.
const char* room() {
    static std::string blanks;
    static float forSize = -1.0f;
    const float size = ImGui::GetFontSize();
    if (size != forSize) {
        forSize = size;
        const float space = std::max(1.0f, ImGui::CalcTextSize(" ").x);
        blanks.assign(std::size_t(std::ceil((size + ImGui::GetStyle().ItemInnerSpacing.x) / space)), ' ');
    }
    return blanks.c_str();
}

} // namespace

void install() { ImGui::GetCurrentContext()->WindowIconHook = hook; }

bool draw(ImDrawList* dl, const char* name, ImVec2 c, float r, ImU32 col) {
    const Pic p = find(name);
    if (p == Pic::None) return false;
    paint(dl, p, c, r, col);
    return true;
}

bool has(const char* name) { return find(name) != Pic::None; }

bool menuItem(const char* label, const char* shortcut, bool selected, bool enabled) {
    const Pic p = find(label);
    if (p == Pic::None) return ImGui::MenuItem(label, shortcut, selected, enabled);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::MenuItemEx(label, room(), shortcut, selected, enabled);
    const float size = ImGui::GetFontSize();
    const float y = 0.5f * (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y);
    paint(ImGui::GetWindowDrawList(), p, ImVec2(at.x + w->DC.MenuColumns.OffsetIcon + size * 0.5f, y),
          radiusFor(size), ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    return clicked;
}

bool menuItem(const char* label, const char* shortcut, bool* flag, bool enabled) {
    if (!menuItem(label, shortcut, flag && *flag, enabled)) return false;
    if (flag) *flag = !*flag;
    return true;
}

bool selectable(const char* label, const char* window, bool selected) {
    const Pic p = find(window);
    if (p == Pic::None) return ImGui::Selectable(label, selected);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Selectable((std::string(room()) + label).c_str(), selected);
    const float size = ImGui::GetFontSize();
    const float y = 0.5f * (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y);
    paint(ImGui::GetWindowDrawList(), p, ImVec2(at.x + size * 0.5f, y), radiusFor(size),
          ImGui::GetColorU32(ImGuiCol_Text));
    return clicked;
}

void comboPreview(const char* label, const char* window) {
    if (!ImGui::BeginComboPreview()) return;
    const Pic p = find(window);
    const float size = ImGui::GetFontSize();
    if (p != Pic::None) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        paint(ImGui::GetWindowDrawList(), p, ImVec2(at.x + size * 0.5f, at.y + size * 0.5f),
              radiusFor(size), ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    }
    ImGui::TextUnformatted(label);
    ImGui::EndComboPreview();
}

} // namespace winicon
