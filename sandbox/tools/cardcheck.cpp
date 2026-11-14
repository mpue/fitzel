// cardcheck -- cards (CardGen.hpp), the lettering on them (Lettering.hpp) and
// the outline filling under both (PolyFill.hpp), without GL.
//
// A letter that is not quite a solid still draws -- and then the slicer
// "repairs" it, or prints a wall through the middle of an "o". So the solids
// are measured: every glyph of several real fonts (TrueType and OpenType)
// filled, its triangles covering exactly what the nonzero sweep covers and
// closing at its outline; the card's plate and relief closed, their volumes
// the areas they stand on; the text not mirrored seen from above; the code on
// the card read back module for module. The layout: the nine anchors and the
// way back from a dragged position, "fit" keeping the text clear of the code,
// the frame narrowing the area, the hole inside the frame. Saved and loaded,
// and through the 3MF export as two closed parts.
//
// Fonts that are not on the machine are skipped (and said so). With an out dir
// it writes the card seen from above as card-top.png.
//   build/linux/bin/cardcheck [outdir]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/CardGen.hpp"
#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditMesh.hpp"
#include "../src/Export3mf.hpp"
#include "../src/Lettering.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/PolyFill.hpp"
#include "../src/SceneGraph.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Closed and wound one way: every directed edge once, and its reverse once.
bool closed(const EditMesh& m, std::string& why) {
    std::map<std::pair<int, int>, int> edges;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 0; k < f.size(); ++k) ++edges[{f[k], f[(k + 1) % f.size()]}];
    for (const auto& [e, n] : edges) {
        const auto back = edges.find({e.second, e.first});
        if (n != 1 || back == edges.end() || back->second != 1) {
            why = "edge " + std::to_string(e.first) + "-" + std::to_string(e.second) + " x" + std::to_string(n);
            return false;
        }
    }
    return !m.faces.empty();
}

double volume(const EditMesh& m) {
    double s = 0.0;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k) {
            const glm::dvec3 a(m.verts[f[0]]), b(m.verts[f[k]]), c(m.verts[f[k + 1]]);
            s += glm::dot(a, glm::cross(b, c));
        }
    return s / 6.0;
}

double polyArea(const std::vector<glm::dvec2>& p) {
    double a = 0.0;
    for (std::size_t i = 0; i < p.size(); ++i)
        a += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
    return 0.5 * a;
}

// The closest two distinct corners (metres); where they are, in `at`.
double closestPair(std::vector<glm::vec3> s, glm::vec3* at = nullptr) {
    std::sort(s.begin(), s.end(), [](const glm::vec3& a, const glm::vec3& b) { return a.x < b.x; });
    double best = 1e30;
    for (std::size_t i = 0; i < s.size(); ++i)
        for (std::size_t j = i + 1; j < s.size() && s[j].x - s[i].x < best; ++j)
            if (static_cast<double>(glm::length(s[j] - s[i])) < best) {
                best = glm::length(s[j] - s[i]);
                if (at) *at = s[i];
            }
    return best;
}

// The faces that face up, from above (x right, -z up): `k` pixels a metre
// from (x0, z0). 255 where nothing.
std::vector<unsigned char> topView(const EditMesh& m, float x0, float z0, int w, int h, float k) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w * h), 255);
    for (const std::vector<int>& f : m.faces) {
        const glm::vec3 a = m.verts[f[0]], b = m.verts[f[1]], c = m.verts[f[2]];
        if (glm::cross(b - a, c - a).y <= 0.0f) continue;
        std::vector<glm::vec2> p;
        for (int i : f) p.emplace_back((m.verts[i].x - x0) * k, (m.verts[i].z - z0) * k);
        glm::vec2 mn(1e9f), mx(-1e9f);
        for (const glm::vec2& q : p) { mn = glm::min(mn, q); mx = glm::max(mx, q); }
        for (int y = std::max(0, static_cast<int>(mn.y)); y < std::min(h, static_cast<int>(mx.y) + 1); ++y)
            for (int x = std::max(0, static_cast<int>(mn.x)); x < std::min(w, static_cast<int>(mx.x) + 1); ++x) {
                const glm::vec2 s(x + 0.5f, y + 0.5f);
                bool in = true;
                for (std::size_t i = 0; i < p.size() && in; ++i) {
                    const glm::vec2 e = p[(i + 1) % p.size()] - p[i], d = s - p[i];
                    in = e.x * d.y - e.y * d.x <= 0.0f;
                }
                if (in) px[static_cast<std::size_t>(y * w + x)] = 0;
            }
    }
    return px;
}

bool closedTris(const std::vector<glm::ivec3>& tris) {
    std::map<std::pair<int, int>, int> edges;
    for (const glm::ivec3& t : tris)
        for (int k = 0; k < 3; ++k) ++edges[{t[k], t[(k + 1) % 3]}];
    for (const auto& [e, n] : edges) {
        const auto back = edges.find({e.second, e.first});
        if (n != 1 || back == edges.end() || back->second != 1) return false;
    }
    return !tris.empty();
}

// Every character a card is likely to carry.
std::string allChars() {
    std::string s;
    for (char c = '!'; c <= '~'; ++c) s += c;
    return s + "\n\xC3\x84\xC3\x96\xC3\x9C\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F\xE2\x82\xAC\xC2\xA7\xC2\xB0"
               "\xC3\xA9\xC3\xA8\xC3\xA7\xE2\x80\x93";
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string out = argc > 1 ? argv[1] : "";
    if (!out.empty()) std::filesystem::create_directories(out);

    // --- Filling outlines ---------------------------------------------------------------
    std::printf("Filling outlines\n");
    {
        using polyfill::DV;
        const std::vector<std::vector<DV>> frame = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}},
                                                    {{3, 3}, {3, 7}, {7, 7}, {7, 3}}};
        polyfill::Fill f;
        const bool filled = polyfill::fill(frame, f);
        check(filled && polyfill::sealed(f) && std::abs(polyfill::area(f) - 84.0) < 1e-9,
              "a square with a square hole: sealed, 84", num(polyfill::area(f)));
        check(std::abs(polyfill::area(polyfill::trapezoids(frame)) - 84.0) < 1e-9, "the sweep agrees");
        const std::vector<std::vector<DV>> two = {{{0, 0}, {6, 0}, {6, 6}, {0, 6}}, {{3, 3}, {9, 3}, {9, 9}, {3, 9}}};
        check(std::abs(polyfill::area(polyfill::trapezoids(two)) - 63.0) < 1e-9,
              "two overlapping squares: the sweep takes their union, 63");
        // The hole of a rounded plate, both ways round.
        const card::Box b{{-40, -25}, {40, 25}};
        std::vector<std::vector<DV>> plate = {card::roundedRect(b, 3.0), card::circle({-34, 19}, 2.5)};
        std::reverse(plate[1].begin(), plate[1].end());
        check(polyfill::fill(plate, f) && polyfill::sealed(f) &&
                  std::abs(polyfill::area(f) - (polyArea(plate[0]) + polyArea(plate[1]))) < 1e-9,
              "a rounded plate with a hole, the hole given either way round");
    }

    // --- Fonts ----------------------------------------------------------------------------
    std::printf("Fonts\n");
    const std::vector<std::string> fonts = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Italic.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/opentype/urw-base35/URWGothic-Book.otf",
        "sandbox/tools/fonts/din1451alt.ttf",
        "../../../sandbox/tools/fonts/din1451alt.ttf",
    };
    int tried = 0;
    for (const std::string& path : fonts) {
        if (!std::filesystem::exists(path)) continue;
        lettering::Font* f = lettering::load(path);
        const std::string name = std::filesystem::path(path).filename().string();
        if (!f) { check(false, name + " reads"); continue; }
        ++tried;
        int glyphs = 0, fell = 0;
        std::string fellList;
        for (char32_t c : std::u32string(U"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                                         U"abcdefghijklmnopqrstuvwxyz{|}~ÄÖÜäöüß€§°éèç–")) {
            const lettering::Shape* s = lettering::shapeOf(*f, c);
            if (!s) continue;
            ++glyphs;
            if (s->fellBack) {
                ++fell;
                char hexc[16];
                std::snprintf(hexc, sizeof hexc, "U+%04X ", static_cast<unsigned>(c));
                fellList += hexc;
            }
        }
        // A card of every character, no code: its relief closed shell by shell.
        card::Params p;
        p.text = allChars();
        p.font = path;
        p.qr = false;
        p.capHeight = 4.0f;
        p.fit = false;
        p.width = 400.0f;
        const card::Model m = card::build(p);
        std::string why;
        const bool ok = closed(m.relief, why);
        check(ok && m.layout.text.missing <= 4,
              name + ": " + std::to_string(glyphs) + " glyphs, " + std::to_string(fell) +
                  " pieced together, all closed",
              (fellList.empty() ? "" : "pieced: " + fellList + "  ") + why + " " +
                  std::to_string(m.layout.text.missing) + " missing");
        glm::vec3 at(0.0f);
        const double gap = closestPair(m.relief.verts, &at);
        check(gap > 1e-6, name + ": no two corners within the 3MF weld",
              num(gap * 1e6) + " um at " + num(at.x * 1000.0) + ", " + num(-at.z * 1000.0) + " mm");
    }
    check(tried >= 2, "at least two fonts found to try", std::to_string(tried));

    const std::string dejavu = lettering::defaultFontPath();
    check(!dejavu.empty() && lettering::load(dejavu), "a default font", dejavu);

    // --- Placing ----------------------------------------------------------------------------
    std::printf("Placing\n");
    {
        const card::Box area{{-30, -20}, {30, 20}};
        const glm::dvec2 size(12, 8);
        bool corners = true, back = true;
        for (int a = 0; a < 9; ++a) {
            const card::Box b = card::place(area, size, {a, 0.0f, 0.0f});
            corners = corners && b.inside(area) && std::abs(b.size().x - 12) < 1e-9 &&
                      (a % 3 != 0 || b.lo.x == area.lo.x) && (a % 3 != 2 || b.hi.x == area.hi.x) &&
                      (a / 3 != 0 || b.hi.y == area.hi.y) && (a / 3 != 2 || b.lo.y == area.lo.y);
            const card::Place moved{a, 3.5f, -2.0f};
            const card::Place again = card::placeAt(area, size, card::place(area, size, moved).lo, 0.0);
            const card::Box b1 = card::place(area, size, moved), b2 = card::place(area, size, again);
            back = back && glm::length(b1.lo - b2.lo) < 1e-5;
        }
        check(corners, "the nine anchors put a thing against the edges they name");
        check(back, "a dragged position comes back as an anchor and an offset that put it there");
        const card::Place snapped = card::placeAt(area, size, card::place(area, size, {8, 0.8f, 0.6f}).lo, 1.5);
        check(snapped == card::Place{8, 0.0f, 0.0f}, "near an anchor it snaps onto it");
    }

    // --- The card ----------------------------------------------------------------------------
    std::printf("The card\n");
    const card::Params def;
    {
        const card::Model m = card::build(def);
        const card::Layout& L = m.layout;
        check(L.warnings.empty(), "the default card: nothing runs into anything",
              L.warnings.empty() ? "" : L.warnings.front());
        check(L.code.valid() && L.textBox.inside(L.area) && L.qrBox.inside(L.area) && !L.textBox.overlaps(L.qrBox),
              "text and code inside the margin, apart");
        check(L.textScale == 1.0, "the default text fits at the size it is set, nothing shrunk", num(L.textScale));
        std::string why;
        check(closed(m.plate, why), "the plate is closed", why);
        check(closed(m.relief, why), "the relief is closed, letter by letter", why);
        const double plateArea = polyArea(card::roundedRect(L.card, def.corner));
        check(std::abs(volume(m.plate) * 1e9 - plateArea * def.thickness) < 1e-3 * plateArea * def.thickness,
              "the plate's volume: the rounded card times its thickness", num(volume(m.plate) * 1e9) + " mm3");
        // The relief: the letters' areas, the code's modules.
        double ink = 0.0;
        for (const lettering::Placed& g : L.text.glyphs) ink += polyfill::area(g.shape->fill) * g.scale * g.scale;
        int dark = 0, diag = 0;
        for (std::uint8_t d : L.code.dark) dark += d;
        for (int j = 0; j <= L.code.size; ++j)
            for (int i = 0; i <= L.code.size; ++i) {
                const bool a = L.code.at(i - 1, j - 1), b = L.code.at(i, j - 1), d = L.code.at(i - 1, j), e = L.code.at(i, j);
                if ((a && e && !b && !d) || (b && d && !a && !e)) ++diag;
            }
        ink += (dark - 2.0 * diag * 0.02) * L.qrPitch * L.qrPitch;
        check(std::abs(volume(m.relief) * 1e9 - ink * def.relief) < 2e-3 * ink * def.relief,
              "the relief's volume: letters and modules times their height",
              num(volume(m.relief) * 1e9) + " mm3, want " + num(ink * def.relief));
        float ylo = 1e9f, yhi = -1e9f;
        for (const glm::vec3& v : m.relief.verts) { ylo = std::min(ylo, v.y); yhi = std::max(yhi, v.y); }
        check(std::abs(ylo * 1000.0f - def.thickness) < 1e-4f && std::abs(yhi * 1000.0f - def.thickness - def.relief) < 1e-4f,
              "the relief stands on the plate", num(ylo * 1000.0) + ".." + num(yhi * 1000.0) + " mm");

        // From above: the code, module for module, where the layout put it.
        const float k = 20000.0f;   // 20 px a mm
        const int w = static_cast<int>(def.width * 20.0f), h = static_cast<int>(def.height * 20.0f);
        const float x0 = -0.0005f * def.width, z0 = -0.0005f * def.height;
        const std::vector<unsigned char> px = topView(m.relief, x0, z0, w, h, k);
        int wrong = 0;
        for (int y = 0; y < L.code.size; ++y)
            for (int x = 0; x < L.code.size; ++x) {
                const double u = L.qrBox.lo.x + (x + 0.5) * L.qrPitch, v = L.qrBox.hi.y - (y + 0.5) * L.qrPitch;
                const int i = static_cast<int>((u / 1000.0 - x0) * k), j = static_cast<int>((-v / 1000.0 - z0) * k);
                wrong += (px[static_cast<std::size_t>(j * w + i)] == 0) != L.code.at(x, y);
            }
        check(wrong == 0, "seen from above the code is where the layout put it, module for module",
              std::to_string(wrong) + " wrong");
        if (!out.empty()) {
            stbi_write_png((out + "/card-top.png").c_str(), w, h, 1, px.data(), w);
            std::printf("     wrote %s/card-top.png\n", out.c_str());
        }
    }
    {
        // Not mirrored: an "L" alone, its stem on the left, its foot at the bottom.
        card::Params p;
        p.text = "L";
        p.qr = false;
        p.capHeight = 30.0f;
        p.textAt = {4, 0.0f, 0.0f};
        const card::Model m = card::build(p);
        const card::Box& b = m.layout.textBox;
        const float k = 20000.0f;
        const int w = static_cast<int>(p.width * 20.0f), h = static_cast<int>(p.height * 20.0f);
        const float x0 = -0.0005f * p.width, z0 = -0.0005f * p.height;
        const std::vector<unsigned char> px = topView(m.relief, x0, z0, w, h, k);
        auto darkAt = [&](double u, double v) {
            const int i = static_cast<int>((u / 1000.0 - x0) * k), j = static_cast<int>((-v / 1000.0 - z0) * k);
            return i >= 0 && j >= 0 && i < w && j < h && px[static_cast<std::size_t>(j * w + i)] == 0;
        };
        // The letter's own extent on the card.
        glm::dvec2 lo(1e9), hi(-1e9);
        const lettering::Placed& g = m.layout.text.glyphs.front();
        for (const glm::dvec2& q : g.shape->fill.points) {
            const glm::dvec2 c = glm::dvec2(b.lo.x, b.hi.y) + g.at + g.scale * q;
            lo = glm::min(lo, c);
            hi = glm::max(hi, c);
        }
        const bool stem = darkAt(lo.x + 1.0, hi.y - 1.0);       // top left: the stem
        const bool notRight = !darkAt(hi.x - 1.0, hi.y - 1.0);  // top right: nothing
        const bool foot = darkAt(hi.x - 1.0, lo.y + 1.0);       // bottom right: the foot
        check(stem && notRight && foot, "an L reads as an L from above (not mirrored, not upside down)");
    }

    // --- Fit, frame, hole -------------------------------------------------------------------
    std::printf("Fit, frame and hole\n");
    {
        card::Params p;
        p.text = "Eine sehr lange Zeile, die nicht neben den Code passt\nund noch eine";
        const card::Layout fitted = card::layoutOf(card::sane(p));
        bool runs = false;
        for (const std::string& w : fitted.warnings) runs = runs || w.find("runs") != std::string::npos;
        check(fitted.textScale < 0.999 && !runs && !fitted.textBox.overlaps(fitted.qrBox),
              "fit shrinks a long text until it keeps clear of the code", num(fitted.textScale));
        p.fit = false;
        const card::Layout over = card::layoutOf(card::sane(p));
        check(over.textScale == 1.0 && !over.warnings.empty(), "without fit it runs on, and says so",
              over.warnings.empty() ? "" : over.warnings.front());

        card::Params f = def;
        f.frame = true;
        f.hole = true;
        f.holeCorner = 1;
        const card::Model m = card::build(f);
        const card::Layout& L = m.layout;
        const double edge = f.frameInset + f.frameWidth;
        check(std::abs(L.area.lo.x - (L.card.lo.x + edge + f.margin)) < 1e-9, "a frame narrows the area by its width");
        const double toFrame = std::min(0.5 * f.width - std::abs(L.holeAt.x), 0.5 * f.height - std::abs(L.holeAt.y));
        check(L.holeAt.x > 0 && L.holeAt.y > 0 && toFrame - L.holeRadius > edge, "the hole sits inside the frame, top right",
              num(L.holeAt.x) + ", " + num(L.holeAt.y));
        std::string why;
        check(closed(m.plate, why) && closed(m.relief, why), "plate (with the hole) and relief (with the frame) closed", why);
        const double holed = polyArea(card::roundedRect(L.card, f.corner)) - polyArea(card::circle(L.holeAt, L.holeRadius));
        check(std::abs(volume(m.plate) * 1e9 - holed * f.thickness) < 1e-3 * holed * f.thickness,
              "the plate's volume less the hole", num(volume(m.plate) * 1e9) + " mm3");
        check(L.warnings.empty(), "nothing runs into anything", L.warnings.empty() ? "" : L.warnings.front());
    }

    // --- Saved and loaded ----------------------------------------------------------------------
    std::printf("Saved and loaded\n");
    {
        card::Params p;
        p.width = 100.0f;
        p.height = 70.0f;
        p.text = "Gäste-WLAN\nscan me";
        p.font = "/some/font.ttf";
        p.align = lettering::Align::Centre;
        p.textAt = {1, 2.5f, -1.0f};
        p.qrAt = {7, 0.0f, 3.0f};
        p.ecc = qrencode::Ecc::High;
        p.frame = true;
        p.hole = true;
        p.holeCorner = 3;
        p.headline = false;
        p.cardColor = {0.1f, 0.2f, 0.3f};
        nlohmann::json j;
        card::saveParams(j, p);
        card::Params q;
        card::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == card::sane(p), "every parameter comes back");
        card::Params d;
        card::loadParams(nlohmann::json::object(), d);
        check(d == card::Params{}, "nothing saved: the defaults");
        std::unique_ptr<ComponentBase> c = components::create("card");
        check(c && dynamic_cast<CardComponent*>(c.get()), "the component is registered as card");
    }

    // --- Through the 3MF export ------------------------------------------------------------------
    std::printf("Through the 3MF export\n");
    {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const card::Palette pal = card::ensurePalette(doc.materials(), def);
        std::vector<Entity> es = card::entities(def, pal, counter, glm::vec3(1.0f, 0.0f, 2.0f));
        check(es.size() == 3 && es[0].components.get<CardComponent>() && es[1].name == "Card" && es[2].name == "Relief",
              "a root keeping the parameters, the card and its relief", es.empty() ? "" : es[0].name);
        const int root = es.empty() ? -1 : es[0].id;
        for (Entity& e : es) doc.entities().push_back(std::move(e));
        scenegraph::resolve(doc.entities());
        const export3mf::Shape s = export3mf::gather(doc, models, {root});
        bool allClosed = s.parts.size() == 2;
        for (const export3mf::Part& part : s.parts) allClosed = allClosed && closedTris(part.tris);
        check(allClosed, "two parts, both closed after the weld");
        check(s.colors.size() == 2, "two colours", std::to_string(s.colors.size()));
        const glm::vec3 mm = export3mf::printSize(s, export3mf::Options{});
        check(std::abs(mm.x - 85.0f) < 1e-3f && std::abs(mm.y - 55.0f) < 1e-3f && std::abs(mm.z - 1.8f) < 1e-3f,
              "85 x 55 x 1.8 mm on the bed", num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z));
        if (!out.empty()) {
            std::string err;
            check(export3mf::write(out + "/card.3mf", s, export3mf::Options{}, err), "written as " + out + "/card.3mf", err);
        }
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
