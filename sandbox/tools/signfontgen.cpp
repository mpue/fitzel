// signfontgen: bakes the street-sign lettering (sandbox/src/SignFont.inc).
//
// Street signs letter their names as GEOMETRY -- see StreetSign.hpp -- so the
// engine needs the outlines of a real typeface, not a texture. This reads a
// TrueType font with stb_truetype (the copy imgui ships), flattens each glyph's
// quadratic curves into polylines, and writes the contours, advances and kerning
// pairs of the characters a street name can contain as a C++ include. The
// runtime then needs no font file, no loader and no rasteriser: the outlines are
// compiled in, and StreetSign turns them into trapezoids.
//
// A one-off tool, re-run only to change the typeface:
//   signfontgen <font.ttf> <out.inc>
// The committed SignFont.inc is "Alte DIN 1451 Mittelschrift" -- the lettering
// of German street signs -- from sandbox/tools/fonts/din1451alt.ttf.

#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Flattening tolerance in font units (typically 1000-2048 per em). On a 9 cm
// capital that is half a millimetre -- well below anything a
// sign is ever looked at from, and it keeps the letters at a few dozen points.
constexpr float kTol = 8.0f;

struct Contour { std::vector<std::pair<int, int>> pts; };
struct Glyph {
    unsigned cp = 0;
    int advance = 0;
    std::vector<Contour> contours;
};

std::vector<unsigned> charset() {
    std::vector<unsigned> cs;
    for (unsigned c = 0x20; c <= 0x7E; ++c) cs.push_back(c);
    for (unsigned c = 0xC0; c <= 0xFF; ++c)
        if (c != 0xD7 && c != 0xF7) cs.push_back(c);   // letters, not x and divide
    cs.push_back(0xB7);     // middle dot
    cs.push_back(0x1E9E);   // capital sharp s
    cs.push_back(0x2013);   // en dash
    cs.push_back(0x2019);   // apostrophe
    return cs;
}

void push(Contour& c, float x, float y) {
    const int ix = static_cast<int>(std::lround(x)), iy = static_cast<int>(std::lround(y));
    if (!c.pts.empty() && c.pts.back().first == ix && c.pts.back().second == iy) return;
    c.pts.emplace_back(ix, iy);
}

Glyph load(const stbtt_fontinfo& f, unsigned cp, int g) {
    Glyph out;
    out.cp = cp;
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&f, g, &adv, &lsb);
    out.advance = adv;
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetGlyphShape(&f, g, &v);
    Contour cur;
    float px = 0.0f, py = 0.0f;
    auto close = [&]() {
        if (cur.pts.size() > 1 && cur.pts.front() == cur.pts.back()) cur.pts.pop_back();
        if (cur.pts.size() >= 3) out.contours.push_back(cur);
        cur.pts.clear();
    };
    for (int i = 0; i < n; ++i) {
        const float x = v[i].x, y = v[i].y;
        switch (v[i].type) {
            case STBTT_vmove:
                close();
                push(cur, x, y);
                break;
            case STBTT_vline:
                push(cur, x, y);
                break;
            case STBTT_vcurve: {
                const float cx = v[i].cx, cy = v[i].cy;
                const float dx = px - 2.0f * cx + x, dy = py - 2.0f * cy + y;
                const float dev = 0.25f * std::sqrt(dx * dx + dy * dy);
                const int segs = std::clamp(static_cast<int>(std::ceil(std::sqrt(dev / kTol))), 1, 16);
                for (int s = 1; s <= segs; ++s) {
                    const float t = static_cast<float>(s) / segs, u = 1.0f - t;
                    push(cur, u * u * px + 2 * u * t * cx + t * t * x,
                              u * u * py + 2 * u * t * cy + t * t * y);
                }
                break;
            }
            case STBTT_vcubic: {
                const float c1x = v[i].cx, c1y = v[i].cy, c2x = v[i].cx1, c2y = v[i].cy1;
                const int segs = 12;
                for (int s = 1; s <= segs; ++s) {
                    const float t = static_cast<float>(s) / segs, u = 1.0f - t;
                    push(cur, u * u * u * px + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * x,
                              u * u * u * py + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * y);
                }
                break;
            }
        }
        px = x; py = y;
    }
    close();
    stbtt_FreeShape(&f, v);
    return out;
}

// A name-table entry (Windows, Unicode BMP, any language) as ASCII.
std::string nameOf(const stbtt_fontinfo& f, int id) {
    for (int lang : {0x409, 0x407, 0}) {
        int len = 0;
        const char* s = stbtt_GetFontNameString(&f, &len, STBTT_PLATFORM_ID_MICROSOFT,
                                                STBTT_MS_EID_UNICODE_BMP, lang, id);
        if (!s) continue;
        std::string out;
        for (int i = 0; i + 1 < len; i += 2)
            if (s[i] == 0 && s[i + 1] >= 0x20 && s[i + 1] != '"') out += s[i + 1];
        return out;
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: signfontgen <font.ttf> <out.inc>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(in)), {});
    if (ttf.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    stbtt_fontinfo f;
    if (!stbtt_InitFont(&f, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
        std::fprintf(stderr, "not a font: %s\n", argv[1]);
        return 1;
    }
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f, &ascent, &descent, &gap);
    int x0, y0, x1, capH = 0, xH = 0;
    stbtt_GetCodepointBox(&f, 'H', &x0, &y0, &x1, &capH);
    stbtt_GetCodepointBox(&f, 'x', &x0, &y0, &x1, &xH);
    const int upem = static_cast<int>(std::lround(1.0f / stbtt_ScaleForMappingEmToPixels(&f, 1.0f)));

    std::vector<Glyph> glyphs;
    std::vector<int>   index;
    for (unsigned cp : charset()) {
        const int g = stbtt_FindGlyphIndex(&f, static_cast<int>(cp));
        if (g == 0 && cp != 0x20) continue;
        glyphs.push_back(load(f, cp, g));
        index.push_back(g);
    }
    struct Kern { unsigned a, b; int adj; };
    std::vector<Kern> kerns;
    for (std::size_t i = 0; i < glyphs.size(); ++i)
        for (std::size_t j = 0; j < glyphs.size(); ++j) {
            const int k = stbtt_GetGlyphKernAdvance(&f, index[i], index[j]);
            if (k != 0) kerns.push_back({glyphs[i].cp, glyphs[j].cp, k});
        }

    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    std::fprintf(o,
        "// GENERATED by sandbox/tools/signfontgen.cpp -- do not edit by hand.\n"
        "// Glyph outlines of \"%s\" (copyright: \"%s\", design: \"%s\").\n"
        "// Curves flattened to %.0f font units. Included by StreetSign.cpp only.\n\n",
        nameOf(f, 1).c_str(), nameOf(f, 0).c_str(), nameOf(f, 8).c_str(), kTol);
    std::fprintf(o, "constexpr int kUnitsPerEm = %d;\nconstexpr int kCapHeight = %d;\n"
                    "constexpr int kXHeight = %d;\nconstexpr int kAscent = %d;\n"
                    "constexpr int kDescent = %d;\n\n", upem, capH, xH, ascent, descent);

    // Points: x, y pairs, contour after contour.
    std::fprintf(o, "const short kPoints[] = {\n");
    std::size_t col = 0, nPts = 0;
    for (const Glyph& g : glyphs)
        for (const Contour& c : g.contours)
            for (const auto& [x, y] : c.pts) {
                std::fprintf(o, "%s%d,%d,", col == 0 ? "    " : "", x, y);
                if (++col == 12) { std::fprintf(o, "\n"); col = 0; }
                ++nPts;
            }
    std::fprintf(o, "%s};\n\n", col ? "\n" : "");

    // Contours: where each one starts in kPoints (in points), plus an end.
    std::fprintf(o, "const unsigned kContourStart[] = {\n");
    col = 0;
    std::size_t at = 0, nContours = 0;
    for (const Glyph& g : glyphs)
        for (const Contour& c : g.contours) {
            std::fprintf(o, "%s%zu,", col == 0 ? "    " : "", at);
            if (++col == 16) { std::fprintf(o, "\n"); col = 0; }
            at += c.pts.size();
            ++nContours;
        }
    std::fprintf(o, "%s%zu,\n};\n\n", col == 0 ? "    " : "", at);

    std::fprintf(o, "struct GlyphDef { unsigned cp; short advance; unsigned first, count; };\n");
    std::fprintf(o, "const GlyphDef kGlyphs[] = {\n");
    std::size_t first = 0;
    for (const Glyph& g : glyphs) {
        std::fprintf(o, "    {0x%04X, %d, %zu, %zu},\n", g.cp, g.advance, first, g.contours.size());
        first += g.contours.size();
    }
    std::fprintf(o, "};\n\n");

    std::fprintf(o, "struct KernDef { unsigned a, b; short adjust; };\n");
    std::fprintf(o, "const KernDef kKerning[] = {\n");
    col = 0;
    for (const Kern& k : kerns) {
        std::fprintf(o, "%s{0x%X,0x%X,%d},", col == 0 ? "    " : "", k.a, k.b, k.adj);
        if (++col == 6) { std::fprintf(o, "\n"); col = 0; }
    }
    if (kerns.empty()) std::fprintf(o, "    {0, 0, 0},");
    std::fprintf(o, "%s};\n", col ? "\n" : "");
    std::fclose(o);
    std::printf("%zu glyphs, %zu contours, %zu points, %zu kerning pairs (upem %d, cap %d)\n",
                glyphs.size(), nContours, nPts, kerns.size(), upem, capH);
    return 0;
}
