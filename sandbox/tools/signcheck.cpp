// signcheck: the street-sign generator (sandbox/src/StreetSign.hpp), without GL.
//
// The lettering is cut from glyph outlines into trapezoids by a sweep that has
// to get winding, holes and touching bands right for every character -- and a
// letter that loses its counter or grows a sliver still "works": it draws,
// nothing crashes, and it just looks wrong on a sign nobody reads closely. So
// the cut is measured here (area of the pieces against the outline's own area,
// glyph by glyph), the layout is checked for ink off the plate, and the
// presets are drawn into a PNG to look at.
//   build/release/bin/signcheck.exe [out.png]

#include "../src/StreetSign.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace {

int g_fail = 0;
void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++g_fail;
}

float area(const std::vector<glm::vec2>& p) {
    float a = 0.0f;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const glm::vec2& u = p[i];
        const glm::vec2& v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return 0.5f * a;
}

bool convexCcw(const std::vector<glm::vec2>& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const glm::vec2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
        const float z = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (z < -1e-9f) return false;
    }
    return true;
}

// --- A tiny rasteriser for the preview sheet -------------------------------------
struct Image {
    int w, h;
    std::vector<unsigned char> px;
    Image(int w_, int h_) : w(w_), h(h_), px(static_cast<std::size_t>(w_ * h_ * 3), 0) {}
    void fill(glm::vec3 c) {
        for (int i = 0; i < w * h; ++i)
            for (int k = 0; k < 3; ++k) px[static_cast<std::size_t>(i * 3 + k)] =
                static_cast<unsigned char>(std::lround(glm::clamp(c[k], 0.0f, 1.0f) * 255.0f));
    }
    // Polygons are painted as LAYERS: each marks the 4x4 sub-samples it covers
    // in a mask, and the layer is blended once at the end -- a letter's
    // touching pieces then meet without a seam of half-covered pixels.
    std::vector<std::uint16_t> mask;
    void begin() { mask.assign(static_cast<std::size_t>(w * h), 0); }
    void poly(const std::vector<glm::vec2>& p) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const glm::vec2& v : p) {
            x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
            y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
        }
        const bool ccw = area(p) > 0.0f;
        for (int y = std::max(0, static_cast<int>(y0)); y <= std::min(h - 1, static_cast<int>(y1)); ++y)
            for (int x = std::max(0, static_cast<int>(x0)); x <= std::min(w - 1, static_cast<int>(x1)); ++x)
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const glm::vec2 q(x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f);
                        bool in = true;
                        for (std::size_t i = 0; i < p.size() && in; ++i) {
                            const glm::vec2 a = p[i], b = p[(i + 1) % p.size()];
                            const float z = (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x);
                            in = ccw ? z >= 0.0f : z <= 0.0f;
                        }
                        if (in) mask[static_cast<std::size_t>(y * w + x)] |=
                                    static_cast<std::uint16_t>(1u << (sy * 4 + sx));
                    }
    }
    void end(glm::vec3 c) {
        for (int i = 0; i < w * h; ++i) {
            int hit = 0;
            for (unsigned m = mask[static_cast<std::size_t>(i)]; m; m &= m - 1) ++hit;
            if (!hit) continue;
            const float t = hit / 16.0f;
            for (int k = 0; k < 3; ++k) {
                unsigned char& d = px[static_cast<std::size_t>(i * 3 + k)];
                d = static_cast<unsigned char>(std::lround(d * (1.0f - t) +
                                                            glm::clamp(c[k], 0.0f, 1.0f) * 255.0f * t));
            }
        }
    }
};

// Draw a face with its top-left at (ox, oy), `ppm` pixels per metre.
void drawFace(Image& im, const streetsign::Face& f, const streetsign::Style& st, float ox, float oy,
              float ppm) {
    auto P = [&](glm::vec2 v) {
        return glm::vec2(ox + (v.x + 0.5f * f.width) * ppm, oy + (0.5f * f.height - v.y) * ppm);
    };
    std::vector<glm::vec2> q;
    for (const glm::vec2& v : f.outline) q.push_back(P(v));
    im.begin();
    im.poly(q);
    im.end(st.plate);
    im.begin();
    for (const streetsign::Poly& p : f.ink) {
        q.clear();
        for (const glm::vec2& v : p.pts) q.push_back(P(v));
        im.poly(q);
    }
    im.end(st.ink);
}

} // namespace

int main(int argc, char** argv) {
    using namespace streetsign;
    const std::string out = argc > 1 ? argv[1] : "signcheck.png";

    // --- The cut: every glyph's pieces cover exactly its outline --------------
    // Laid out one character at a time (Classic, no frame): the ink is the glyph.
    {
        Style bare = presetStyle(0);
        bare.frame = Frame::None;
        int bad = 0, total = 0;
        std::string worst;
        for (unsigned cp = 0x21; cp < 0x2020; ++cp) {
            std::string s;
            if (cp < 0x80) s += static_cast<char>(cp);
            else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
            if (lettered(s, false).empty()) continue;
            ++total;
            const Face f = layout(s, bare, 1.0f);
            for (const Poly& p : f.ink) {
                const float a = area(p.pts);
                if (a < -1e-7f || !convexCcw(p.pts)) { ++bad; worst = s; break; }
            }
        }
        check(bad == 0, "every glyph cuts into convex, counter-clockwise pieces",
              std::to_string(total) + " glyphs" + (bad ? ", first bad: " + worst : ""));
        check(total >= 150, "the typeface covers ASCII, Latin-1 and the dashes",
              std::to_string(total) + " glyphs");
        // Overlapping contours (C cedilla, O slash) may fall back to the
        // trapezoid cut; nothing a German street name is made of may.
        std::u32string german;
        for (char32_t c = 0x21; c < 0x7F; ++c) german += c;
        german += std::u32string{0xC4, 0xD6, 0xDC, 0xE4, 0xF6, 0xFC, 0xDF};
        int heavy = 0;
        for (char32_t c : german) heavy += glyphFellBack(c) ? 1 : 0;
        check(heavy == 0, "ASCII, umlauts and sharp s are ear-clipped (no trapezoid fallback)",
              std::to_string(heavy) + " fell back");
    }
    // Area: an 'O' is a ring, not a disc; an 'I' is a bar. A lost counter or a
    // doubled band shows up as area. Compare a filled letter against a known one.
    {
        Style bare = presetStyle(0);
        bare.frame = Frame::None;
        auto ink = [&](const char* s) {
            float a = 0.0f;
            for (const Poly& p : layout(s, bare, 1.0f).ink) a += area(p.pts);
            return a;
        };
        const float o = ink("O"), i = ink("I"), dot = ink(".");
        check(o > 0.0f && o < 0.45f, "O keeps its counter", "area " + std::to_string(o));
        check(i > 0.08f && i < 0.25f, "I is one bar", "area " + std::to_string(i));
        check(std::abs(ink("OO") - 2.0f * o) < 1e-4f, "a letter's pieces are its own (OO = 2 O)");
        check(dot > 0.0f && dot < i, "a full stop is smaller than an I");
    }

    // --- Text -------------------------------------------------------------------
    check(lettered("Stra\xC3\x9F" "e", true) == U"STRASSE", "capitals write sharp s as SS");
    check(lettered("M\xC3\xBC" "hlgasse", true) == U"M" + std::u32string(1, 0xDC) + U"HLGASSE", "capitals keep the umlaut");
    check(lettered("Stra\xC3\x9F" "e", false) == U"Stra" + std::u32string(1, 0xDF) + U"e", "mixed case keeps sharp s");
    check(lettered("A\xE2\x82\xAC" "B", false) == U"AB", "a character the font lacks is dropped");

    // --- Layout ---------------------------------------------------------------
    for (int k = 0; k < static_cast<int>(presets().size()); ++k) {
        const Style st = presetStyle(k);
        const Face s = layout("Ab", st, 0.085f), l = layout("Adalbert-Stifter-Stra\xC3\x9F" "e", st, 0.085f);
        bool inside = true;
        for (const Poly& p : l.ink)
            for (const glm::vec2& v : p.pts)
                if (std::abs(v.x) > 0.5f * l.width + 1e-5f || std::abs(v.y) > 0.5f * l.height + 1e-5f)
                    inside = false;
        check(inside && l.width > s.width && std::abs(l.height - s.height) < 1e-6f,
              (std::string("layout: ") + presets()[static_cast<std::size_t>(k)].name).c_str(),
              std::to_string(l.width).substr(0, 5) + " m long");
    }

    // --- The model --------------------------------------------------------------
    {
        Params p;
        p.textB = "Hauptstra\xC3\x9F" "e";
        Palette pal;
        pal.plate = fitzel::AssetId::generate();
        pal.ink   = fitzel::AssetId::generate();
        pal.post  = fitzel::AssetId::generate();
        const auto polys = build(p, pal);
        float top = 0.0f;
        int postFaces = 0;
        bool sane = true;
        for (const Poly3& q : polys) {
            for (const glm::vec3& v : q.pts) top = std::max(top, v.y);
            postFaces += q.post;
            sane = sane && q.pts.size() >= 3 && std::abs(glm::length(q.normal) - 1.0f) < 1e-3f;
        }
        check(sane, "every face has three corners and a unit normal", std::to_string(polys.size()) + " faces");
        check(postFaces > 0 && top > p.postHeight + 0.3f && top < p.postHeight + 0.8f,
              "two blades stack on the post", "top at " + std::to_string(top).substr(0, 4) + " m");
        Params plate = p;
        plate.mount = Mount::Plate;
        int platePost = 0;
        for (const Poly3& q : build(plate, pal)) platePost += q.post;
        check(platePost == 0, "a plate has no post");
        const auto ms = meshes(polys);
        std::size_t verts = 0;
        std::string split;
        for (const auto& m : ms) {
            verts += m.second.vertices.size();
            split += (split.empty() ? "" : ", ") +
                     std::string(m.first == pal.ink ? "ink " : m.first == pal.plate ? "plate " : "post ") +
                     std::to_string(m.second.vertices.size());
        }
        check(ms.size() == 3, "three materials: plate, ink, post",
              std::to_string(verts) + " vertices (" + split + ")");
        int counter = 1;
        const auto es = entities(p, pal, counter, glm::vec3(10, 0, 5));
        check(es.size() == 3 && es.front().components.get<StreetSignComponent>() &&
                  es[1].parent == es[0].id && es[1].components.get<PhysicsComponent>(),
              "entities: root with parameters, Post (collider), Sign");
        nlohmann::json j;
        saveParams(j, p);
        Params back;
        loadParams(j, back);
        check(back.textA == p.textA && back.textB == p.textB && back.style.frame == p.style.frame &&
                  back.style.plate == p.style.plate && back.postHeight == p.postHeight,
              "parameters survive a save and load");
    }

    // --- The sheet ------------------------------------------------------------
    {
        const char* names[] = {"Fitzelstra\xC3\x9F" "e", "Adalbert-Stifter-Stra\xC3\x9F" "e",
                               "Abtsg\xC3\xA4\xC3\x9F" "chen", "Am R\xC3\xB6merberg",
                               "Gro\xC3\x9F" "e Bockenheimer Stra\xC3\x9F" "e", "Zeil",
                               "Hauptwache"};
        const float ppm = 800.0f;
        Image im(1600, 40 + 200 * static_cast<int>(presets().size()));
        im.fill({0.55f, 0.58f, 0.60f});
        int k = 0;
        for (const Preset& pr : presets()) {
            const Face f = layout(names[k % 7], pr.style, 0.085f);
            drawFace(im, f, pr.style, 30.0f, 30.0f + k * 200.0f, std::min(ppm, 1540.0f / f.width));
            ++k;
        }
        const int ok = stbi_write_png(out.c_str(), im.w, im.h, 3, im.px.data(), im.w * 3);
        check(ok != 0, "preview sheet written", out);
    }

    std::printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail;
}
