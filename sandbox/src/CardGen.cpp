#include "CardGen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

#include "PolyFill.hpp"
#include "QrCode.hpp"

using fitzel::AssetId;

namespace card {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Card millimetres (y up, middle at the origin) to the scene: metres, lying
// flat, the card's top towards -Z.
glm::vec3 world(glm::dvec2 c, double y) {
    return glm::vec3(static_cast<float>(c.x / 1000.0), static_cast<float>(y / 1000.0),
                     static_cast<float>(-c.y / 1000.0));
}

Box inset(const Box& b, double d) { return {b.lo + glm::dvec2(d), b.hi - glm::dvec2(d)}; }

bool touchesCircle(const Box& b, glm::dvec2 c, double r) {
    const glm::dvec2 q = glm::clamp(c, b.lo, b.hi);
    return glm::length(q - c) < r;
}

// A filled outline (sealed: see PolyFill.hpp) as a closed solid from height
// y0 to y1 (mm): its triangles on top and turned over below, a wall along
// every outline edge.
void addSolid(EditMesh& m, const polyfill::Fill& f, const std::function<glm::dvec2(glm::dvec2)>& to,
              double y0, double y1) {
    const int base = static_cast<int>(m.verts.size());
    for (const glm::dvec2& p : f.points) {
        const glm::dvec2 c = to(p);
        m.verts.push_back(world(c, y0));
        m.verts.push_back(world(c, y1));
    }
    auto bot = [&](int i) { return base + 2 * i; };
    auto top = [&](int i) { return base + 2 * i + 1; };
    for (const glm::ivec3& t : f.tris) {
        m.faces.push_back({top(t.x), top(t.y), top(t.z)});
        m.faces.push_back({bot(t.x), bot(t.z), bot(t.y)});
    }
    for (const std::vector<int>& l : f.loops)
        for (std::size_t k = 0; k < l.size(); ++k) {
            const int p = l[k], q = l[(k + 1) % l.size()];
            m.faces.push_back({bot(q), top(q), top(p), bot(p)});
        }
}

// A band of a glyph (font units, `to` taking it onto the card) as a prism of
// its own, drawn in by `inset` mm on every side so it shares no corner with
// the band above, below or beside it -- the 3MF export would weld those, and
// an edge of four walls is no solid. What a glyph whose outlines overlap is
// built from; a slicer closes the hair-wide seams. Bands too thin to draw in
// are left out.
void addBand(EditMesh& m, const polyfill::Trap& t, const std::function<glm::dvec2(glm::dvec2)>& to, double y0,
             double y1, double inset) {
    const glm::dvec2 a = to({t.xl0, t.y0}), b = to({t.xr0, t.y0}), c = to({t.xr1, t.y1}), d = to({t.xl1, t.y1});
    if (c.y - a.y <= 4.0 * inset) return;
    const double lo = a.y + inset, hi = c.y - inset;
    auto along = [](glm::dvec2 p, glm::dvec2 q, double y) { return p.x + (q.x - p.x) * (y - p.y) / (q.y - p.y); };
    double l0 = along(a, d, lo) + inset, r0 = along(b, c, lo) - inset;
    double l1 = along(a, d, hi) + inset, r1 = along(b, c, hi) - inset;
    if (r0 - l0 < 2.0 * inset && r1 - l1 < 2.0 * inset) return;
    if (r0 < l0) l0 = r0 = 0.5 * (l0 + r0);   // a band that ends in a point
    if (r1 < l1) l1 = r1 = 0.5 * (l1 + r1);
    std::vector<glm::dvec2> pts = {{l0, lo}, {r0, lo}, {r1, hi}, {l1, hi}};
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](const glm::dvec2& p, const glm::dvec2& q) { return glm::length(p - q) < 1e-3; }),
              pts.end());
    if (pts.size() > 1 && glm::length(pts.front() - pts.back()) < 1e-3) pts.pop_back();
    if (pts.size() < 3) return;
    polyfill::Fill f;
    f.loops.emplace_back();
    for (std::size_t i = 0; i < pts.size(); ++i) {
        f.points.push_back(pts[i]);
        f.loops.back().push_back(static_cast<int>(i));
        if (i >= 2) f.tris.emplace_back(0, static_cast<int>(i - 1), static_cast<int>(i));
    }
    addSolid(m, f, [](glm::dvec2 p) { return p; }, y0, y1);
}

void append(EditMesh& dst, const EditMesh& src, const glm::vec3& shift) {
    const int base = static_cast<int>(dst.verts.size());
    for (const glm::vec3& v : src.verts) dst.verts.push_back(v + shift);
    for (const std::vector<int>& f : src.faces) {
        std::vector<int> g = f;
        for (int& i : g) i += base;
        dst.faces.push_back(std::move(g));
    }
}

double luminance(const glm::vec3& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

AssetId ensure(std::vector<MaterialDef>& mats, const std::string& name, glm::vec3 albedo) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = 0.0f;
    md.roughness    = 0.55f;   // printed plastic
    mats.push_back(md);
    return md.assetId;
}

} // namespace

// --- Parameters -------------------------------------------------------------------

Params sane(const Params& p) {
    Params s = p;
    s.width     = std::clamp(p.width, 10.0f, 2000.0f);
    s.height    = std::clamp(p.height, 10.0f, 2000.0f);
    const float shortSide = std::min(s.width, s.height);
    s.thickness = std::clamp(p.thickness, 0.2f, 50.0f);
    s.corner    = std::clamp(p.corner, 0.0f, 0.5f * shortSide - 0.01f);
    s.margin    = std::clamp(p.margin, 0.0f, 0.5f * shortSide - 2.0f);
    s.relief    = std::clamp(p.relief, 0.05f, 20.0f);
    s.capHeight = std::clamp(p.capHeight, 0.5f, 300.0f);
    s.headlineScale = std::clamp(p.headlineScale, 1.0f, 4.0f);
    s.spacing   = std::clamp(p.spacing, 0.6f, 3.0f);
    s.align     = static_cast<lettering::Align>(std::clamp(static_cast<int>(p.align), 0,
                                                           static_cast<int>(lettering::Align::Count) - 1));
    s.textAt.anchor = std::clamp(p.textAt.anchor, 0, 8);
    s.qrAt.anchor   = std::clamp(p.qrAt.anchor, 0, 8);
    s.ecc       = static_cast<qrencode::Ecc>(std::clamp(static_cast<int>(p.ecc), 0,
                                                        static_cast<int>(qrencode::Ecc::Count) - 1));
    s.qrSize    = std::clamp(p.qrSize, 5.0f, shortSide);
    s.frameWidth = std::clamp(p.frameWidth, 0.2f, 0.25f * shortSide);
    s.frameInset = std::clamp(p.frameInset, 0.0f, 0.25f * shortSide);
    s.holeDiameter = std::clamp(p.holeDiameter, 1.0f, 0.4f * shortSide);
    s.holeCorner = std::clamp(p.holeCorner, 0, 3);
    return s;
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> ps = {
        {"Business card", 85.0f, 55.0f, 3.0f},
        {"Credit card", 85.6f, 53.98f, 3.18f},
        {"Postcard A6", 148.0f, 105.0f, 2.0f},
        {"Square", 70.0f, 70.0f, 4.0f},
        {"Tag", 45.0f, 75.0f, 5.0f},
        {"Bookmark", 50.0f, 160.0f, 4.0f},
    };
    return ps;
}

// --- Placing --------------------------------------------------------------------------

Box place(const Box& area, glm::dvec2 size, const Place& at) {
    const int col = std::clamp(at.anchor, 0, 8) % 3, row = std::clamp(at.anchor, 0, 8) / 3;
    double x = col == 0 ? area.lo.x : col == 1 ? area.centre().x - 0.5 * size.x : area.hi.x - size.x;
    double top = row == 0 ? area.hi.y : row == 1 ? area.centre().y + 0.5 * size.y : area.lo.y + size.y;
    x += at.dx;
    top -= at.dy;
    return {{x, top - size.y}, {x + size.x, top}};
}

Place placeAt(const Box& area, glm::dvec2 size, glm::dvec2 lo, double snap) {
    Place best;
    double bestD = 1e300;
    for (int a = 0; a < 9; ++a) {
        const glm::dvec2 d = lo - place(area, size, {a, 0.0f, 0.0f}).lo;
        if (glm::length(d) < bestD) {
            bestD = glm::length(d);
            best = {a, static_cast<float>(d.x), static_cast<float>(-d.y)};
        }
    }
    if (bestD <= snap) best.dx = best.dy = 0.0f;
    return best;
}

std::vector<glm::dvec2> roundedRect(const Box& b, double r) {
    r = std::clamp(r, 0.0, 0.5 * std::min(b.size().x, b.size().y));
    if (r < 1e-6) return {b.lo, {b.hi.x, b.lo.y}, b.hi, {b.lo.x, b.hi.y}};
    const int seg = std::clamp(static_cast<int>(std::ceil(r * 4.0)), 4, 24);
    const glm::dvec2 cs[4] = {{b.hi.x - r, b.lo.y + r}, {b.hi.x - r, b.hi.y - r},
                              {b.lo.x + r, b.hi.y - r}, {b.lo.x + r, b.lo.y + r}};
    std::vector<glm::dvec2> out;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= seg; ++i) {
            const double a = (k - 1) * 0.5 * kPi + 0.5 * kPi * i / seg;   // -90 deg at the bottom right
            out.push_back(cs[k] + r * glm::dvec2(std::cos(a), std::sin(a)));
        }
    return out;
}

std::vector<glm::dvec2> circle(glm::dvec2 c, double r) {
    const int seg = std::clamp(static_cast<int>(std::ceil(r * 12.0)), 24, 96);
    std::vector<glm::dvec2> out;
    for (int i = 0; i < seg; ++i) {
        const double a = 2.0 * kPi * i / seg;
        out.push_back(c + r * glm::dvec2(std::cos(a), std::sin(a)));
    }
    return out;
}

// --- Layout ---------------------------------------------------------------------------

Layout layoutOf(const Params& p) {
    Layout L;
    L.card = {{-0.5 * p.width, -0.5 * p.height}, {0.5 * p.width, 0.5 * p.height}};
    const double edge = p.frame ? p.frameInset + p.frameWidth : 0.0;
    L.area = inset(L.card, edge + p.margin);
    if (L.area.size().x < 1.0 || L.area.size().y < 1.0) L.area = inset(L.card, 0.25 * std::min(p.width, p.height));

    if (p.hole) {
        L.holeRadius = 0.5 * p.holeDiameter;
        const double in = std::max(L.holeRadius + 1.5, p.frame ? edge + L.holeRadius + 1.0 : 0.0);
        L.holeAt = {(p.holeCorner % 2 ? 1.0 : -1.0) * (0.5 * p.width - in),
                    (p.holeCorner / 2 ? -1.0 : 1.0) * (0.5 * p.height - in)};
    }

    if (p.qr && !p.qrText.empty()) {
        L.code = qrencode::encode(p.qrText, p.ecc);
        if (L.code.valid()) {
            L.qrPitch = p.qrSize / static_cast<double>(L.code.size);
            L.qrBox = place(L.area, glm::dvec2(p.qrSize), p.qrAt);
        } else {
            L.warnings.push_back("The code's text is too long for a QR code.");
        }
    }
    // Light round the code: two modules of it, and never under a millimetre.
    const double quiet = std::max(2.0 * L.qrPitch, 1.0);

    lettering::Font* font = nullptr;
    L.fontPath = p.font.empty() ? lettering::defaultFontPath() : p.font;
    if (!L.fontPath.empty()) font = lettering::load(L.fontPath);
    if (!font && !p.font.empty()) {
        L.warnings.push_back("The font could not be read; the default one stands in.");
        L.fontPath = lettering::defaultFontPath();
        if (!L.fontPath.empty()) font = lettering::load(L.fontPath);
    }
    if (!font) {
        L.fontPath.clear();
        if (!p.text.empty()) L.warnings.push_back("No font found to set the text in.");
    }

    if (font && !p.text.empty()) {
        lettering::Style st;
        st.capHeight = p.capHeight;
        st.headline  = p.headline ? p.headlineScale : 1.0f;
        st.spacing   = p.spacing;
        st.align     = p.align;
        const lettering::Block full = lettering::layout(*font, p.text, st);
        const glm::dvec2 size(full.width, full.height);
        auto clear = [&](double s) {
            const Box b = place(L.area, size * s, p.textAt);
            if (!b.inside(L.area)) return false;
            if (L.code.valid() && b.overlaps(L.qrBox, quiet)) return false;
            if (L.holeRadius > 0.0 && touchesCircle(b, L.holeAt, L.holeRadius + 1.0)) return false;
            return true;
        };
        double s = 1.0;
        if (p.fit && !clear(1.0)) {
            double lo = 0.1, hi = 1.0;
            if (clear(lo)) {
                for (int i = 0; i < 30; ++i) {
                    const double m = 0.5 * (lo + hi);
                    (clear(m) ? lo : hi) = m;
                }
            }
            s = lo * 0.999;   // off the boundary, which a rounding can step over
        }
        L.textScale = s;
        st.capHeight = static_cast<float>(p.capHeight * s);
        L.text = s == 1.0 ? full : lettering::layout(*font, p.text, st);
        L.textBox = place(L.area, glm::dvec2(L.text.width, L.text.height), p.textAt);
        if (!L.textBox.inside(L.area)) L.warnings.push_back("The text runs past the margin.");
        if (L.code.valid() && L.textBox.overlaps(L.qrBox, quiet))
            L.warnings.push_back("The text runs into the code (or its quiet zone).");
        if (L.holeRadius > 0.0 && touchesCircle(L.textBox, L.holeAt, L.holeRadius + 1.0))
            L.warnings.push_back("The text runs into the hole.");
        if (st.capHeight < 2.0f)
            L.warnings.push_back("Letters under 2 mm are finer than a 0.4 mm nozzle prints cleanly.");
        if (L.text.missing > 0)
            L.warnings.push_back(std::to_string(L.text.missing) + " characters are not in this font.");
    }

    if (L.code.valid()) {
        if (!L.qrBox.inside(L.area)) L.warnings.push_back("The code runs past the margin.");
        if (L.holeRadius > 0.0 && touchesCircle(L.qrBox, L.holeAt, L.holeRadius + quiet))
            L.warnings.push_back("The code runs into the hole.");
        if (L.qrPitch < 0.8)
            L.warnings.push_back("The code's modules are under 0.8 mm: make it bigger or its text shorter.");
        if (luminance(p.inkColor) > luminance(p.cardColor))
            L.warnings.push_back("Light on dark: not every phone reads a code that way round.");
    }
    return L;
}

// --- The model ------------------------------------------------------------------------

Model build(const Params& in) {
    const Params p = sane(in);
    Model m;
    m.layout = layoutOf(p);
    const Layout& L = m.layout;
    const double T = p.thickness, R = p.relief;
    const auto same = [](glm::dvec2 v) { return v; };

    // The plate, with its hole.
    {
        std::vector<std::vector<glm::dvec2>> outline = {roundedRect(L.card, p.corner)};
        if (L.holeRadius > 0.0) outline.push_back(circle(L.holeAt, L.holeRadius));
        polyfill::Fill f;
        if (!polyfill::fill(outline, f, /*seal=*/true)) {
            outline.resize(1);   // never seen; a card without its hole rather than none
            polyfill::fill(outline, f, true);
        }
        addSolid(m.plate, f, same, 0.0, T);
    }
    // The frame: a ring, its corners following the card's.
    if (p.frame) {
        const Box outer = inset(L.card, p.frameInset), inner = inset(outer, p.frameWidth);
        polyfill::Fill f;
        if (polyfill::fill({roundedRect(outer, p.corner - p.frameInset),
                            roundedRect(inner, p.corner - p.frameInset - p.frameWidth)}, f, /*seal=*/true))
            addSolid(m.relief, f, same, T, T + R);
    }
    // The code.
    if (L.code.valid()) {
        const EditMesh q = qrcode::moduleSolids(L.code, static_cast<float>(L.qrPitch / 1000.0),
                                                static_cast<float>(T / 1000.0), static_cast<float>((T + R) / 1000.0));
        append(m.relief, q, world(L.qrBox.centre(), 0.0));
    }
    // The text: the block's (0, 0) is the top left of its box.
    const glm::dvec2 origin(L.textBox.lo.x, L.textBox.hi.y);
    for (const lettering::Placed& g : L.text.glyphs) {
        auto to = [&](glm::dvec2 u) { return origin + g.at + g.scale * u; };
        if (!g.shape->fill.tris.empty()) {
            addSolid(m.relief, g.shape->fill, to, T, T + R);
        } else {
            for (const polyfill::Trap& t : g.shape->traps) addBand(m.relief, t, to, T, T + R, 0.005);
        }
    }
    return m;
}

Palette ensurePalette(std::vector<MaterialDef>& mats, const Params& p) {
    auto colour = [&](const char* part, glm::vec3 c) {
        char name[64];
        std::snprintf(name, sizeof name, "Card %s#%02X%02X%02X", part, toByte(c.r), toByte(c.g), toByte(c.b));
        return ensure(mats, name, c);
    };
    Palette pal;
    pal.card = colour("", p.cardColor);
    pal.ink  = colour("Ink ", p.inkColor);
    return pal;
}

std::string autoName(const Params& p) {
    std::string t = p.text.substr(0, p.text.find('\n'));
    if (t.size() > 24) {
        std::size_t k = 24;
        while (k > 0 && (static_cast<unsigned char>(t[k]) & 0xC0) == 0x80) --k;
        t = t.substr(0, k) + "...";
    }
    return t.empty() ? "Card" : "Card " + t;
}

std::vector<Entity> entities(const Params& in, const Palette& pal, int& counter, const glm::vec3& at) {
    const Params p = sane(in);
    Model mdl = build(p);
    std::vector<Entity> out;
    Entity root;
    root.type        = EntityType::Empty;
    root.name        = autoName(p);
    root.id          = counter++;
    root.parent      = -1;
    root.half        = glm::vec3(0.5f * p.width, 0.5f * (p.thickness + p.relief), 0.5f * p.height) / 1000.0f;
    root.localCenter = root.center = at;
    {
        auto cc = std::make_unique<CardComponent>();
        cc->params = p;
        root.components.items.push_back(std::move(cc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;

    // One child per part, re-centred on its own bounds (the Modeling panel's
    // invariant: pick box, gizmo and collider describe the shape that is there).
    auto add = [&](const char* name, EditMesh m, AssetId mat, bool collide) {
        if (m.faces.empty()) return;
        const glm::vec3 shift = editmesh::recenter(m);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        Entity e;
        e.type        = EntityType::Box;
        e.name        = name;
        e.id          = counter++;
        e.parent      = rootId;
        e.half        = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
        e.localCenter = shift;
        e.center      = at + shift;
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = std::move(m);
        mc->touch();
        e.components.items.push_back(std::move(mc));
        auto mt = std::make_unique<MaterialComponent>();
        mt->material = mat;
        e.components.items.push_back(std::move(mt));
        if (collide) {
            auto pc = std::make_unique<PhysicsComponent>();
            pc->dynamic = false;
            e.components.items.push_back(std::move(pc));
        }
        out.push_back(std::move(e));
    };
    add("Card", std::move(mdl.plate), pal.card, true);
    add("Relief", std::move(mdl.relief), pal.ink, false);
    return out;
}

// --- Persistence ----------------------------------------------------------------------

namespace {
nlohmann::json colJson(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonCol(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
nlohmann::json placeJson(const Place& p) { return {{"anchor", p.anchor}, {"dx", p.dx}, {"dy", p.dy}}; }
Place jsonPlace(const nlohmann::json& j, const Place& def) {
    if (!j.is_object()) return def;
    return {j.value("anchor", def.anchor), j.value("dx", def.dx), j.value("dy", def.dy)};
}
} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["width"] = p.width;
    j["height"] = p.height;
    j["thickness"] = p.thickness;
    j["corner"] = p.corner;
    j["margin"] = p.margin;
    j["relief"] = p.relief;
    j["text"] = p.text;
    j["font"] = p.font;
    j["capHeight"] = p.capHeight;
    j["headline"] = p.headline;
    j["headlineScale"] = p.headlineScale;
    j["spacing"] = p.spacing;
    j["align"] = static_cast<int>(p.align);
    j["fit"] = p.fit;
    j["textAt"] = placeJson(p.textAt);
    j["qr"] = p.qr;
    j["qrText"] = p.qrText;
    j["ecc"] = static_cast<int>(p.ecc);
    j["qrSize"] = p.qrSize;
    j["qrAt"] = placeJson(p.qrAt);
    j["frame"] = p.frame;
    j["frameWidth"] = p.frameWidth;
    j["frameInset"] = p.frameInset;
    j["hole"] = p.hole;
    j["holeDiameter"] = p.holeDiameter;
    j["holeCorner"] = p.holeCorner;
    j["cardColor"] = colJson(p.cardColor);
    j["inkColor"] = colJson(p.inkColor);
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.width = j.value("width", d.width);
    p.height = j.value("height", d.height);
    p.thickness = j.value("thickness", d.thickness);
    p.corner = j.value("corner", d.corner);
    p.margin = j.value("margin", d.margin);
    p.relief = j.value("relief", d.relief);
    p.text = j.value("text", d.text);
    p.font = j.value("font", d.font);
    p.capHeight = j.value("capHeight", d.capHeight);
    p.headline = j.value("headline", d.headline);
    p.headlineScale = j.value("headlineScale", d.headlineScale);
    p.spacing = j.value("spacing", d.spacing);
    p.align = static_cast<lettering::Align>(j.value("align", static_cast<int>(d.align)));
    p.fit = j.value("fit", d.fit);
    p.textAt = jsonPlace(j.value("textAt", nlohmann::json()), d.textAt);
    p.qr = j.value("qr", d.qr);
    p.qrText = j.value("qrText", d.qrText);
    p.ecc = static_cast<qrencode::Ecc>(j.value("ecc", static_cast<int>(d.ecc)));
    p.qrSize = j.value("qrSize", d.qrSize);
    p.qrAt = jsonPlace(j.value("qrAt", nlohmann::json()), d.qrAt);
    p.frame = j.value("frame", d.frame);
    p.frameWidth = j.value("frameWidth", d.frameWidth);
    p.frameInset = j.value("frameInset", d.frameInset);
    p.hole = j.value("hole", d.hole);
    p.holeDiameter = j.value("holeDiameter", d.holeDiameter);
    p.holeCorner = j.value("holeCorner", d.holeCorner);
    p.cardColor = jsonCol(j.value("cardColor", nlohmann::json()), d.cardColor);
    p.inkColor = jsonCol(j.value("inkColor", nlohmann::json()), d.inkColor);
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved card come back as
// a card, parameters and all.
struct RegisterCard {
    RegisterCard() {
        components::registerType({"card", "Card",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CardComponent>()); },
            /*addable=*/false});
    }
} g_registerCard;
} // namespace

} // namespace card
