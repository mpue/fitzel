#include "Lettering.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <unordered_map>

// A private copy: the editor's image tools carry their own, and the player
// has none.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

namespace lettering {

struct Font {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    int    ascent = 0, descent = 0, lineGap = 0;
    double capUnits = 700.0;   // a capital's height, font units
    double tolerance = 1.0;    // how far a cut curve may stray, font units
    std::unordered_map<int, Shape> shapes;   // by glyph index
};

namespace {

using polyfill::DV;

// A curve cut into enough straight pieces that none strays more than `tol`
// from it: a quadratic's (or cubic's) error falls with the square of the count.
int pieces(double deviation, double tol) {
    return std::clamp(static_cast<int>(std::ceil(std::sqrt(deviation / std::max(tol, 1e-9)))), 1, 32);
}

std::vector<std::vector<DV>> outlinesOf(const Font& f, int glyph) {
    std::vector<std::vector<DV>> cs;
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetGlyphShape(&f.info, glyph, &v);
    DV cur(0.0);
    for (int i = 0; i < n; ++i) {
        const DV p(v[i].x, v[i].y);
        switch (v[i].type) {
        case STBTT_vmove:
            cs.emplace_back();
            cs.back().push_back(p);
            break;
        case STBTT_vline:
            if (!cs.empty()) cs.back().push_back(p);
            break;
        case STBTT_vcurve: {
            if (cs.empty()) break;
            const DV c(v[i].cx, v[i].cy);
            const int k = pieces(0.25 * glm::length(cur - 2.0 * c + p), f.tolerance);
            for (int s = 1; s <= k; ++s) {
                const double t = static_cast<double>(s) / k, u = 1.0 - t;
                cs.back().push_back(u * u * cur + 2.0 * u * t * c + t * t * p);
            }
            break;
        }
        case STBTT_vcubic: {
            if (cs.empty()) break;
            const DV c1(v[i].cx, v[i].cy), c2(v[i].cx1, v[i].cy1);
            const double d = 0.75 * std::max(glm::length(cur - 2.0 * c1 + c2), glm::length(c1 - 2.0 * c2 + p));
            const int k = pieces(d, f.tolerance);
            for (int s = 1; s <= k; ++s) {
                const double t = static_cast<double>(s) / k, u = 1.0 - t;
                cs.back().push_back(u * u * u * cur + 3.0 * u * u * t * c1 + 3.0 * u * t * t * c2 + t * t * t * p);
            }
            break;
        }
        default:
            break;
        }
        cur = p;
    }
    if (v) stbtt_FreeShape(&f.info, v);
    return cs;
}

const Shape* shapeFor(Font& f, int glyph) {
    auto it = f.shapes.find(glyph);
    if (it != f.shapes.end()) return &it->second;
    Shape s;
    const std::vector<std::vector<DV>> outlines = outlinesOf(f, glyph);
    if (!outlines.empty()) {
        // Triangles when they cover exactly what the sweep covers and close
        // into a solid; otherwise the sweep's trapezoids.
        s.traps = polyfill::trapezoids(outlines);
        const double want = polyfill::area(s.traps);
        if (polyfill::fill(outlines, s.fill, /*seal=*/true) &&
            std::abs(polyfill::area(s.fill) - want) <= 1e-4 * std::max(want, 1.0)) {
            s.traps.clear();
        } else {
            s.fill     = polyfill::Fill{};
            s.fellBack = !s.traps.empty();
        }
    }
    return &f.shapes.emplace(glyph, std::move(s)).first->second;
}

std::vector<std::u32string> linesOf(const std::string& s) {
    std::vector<std::u32string> lines(1);
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        int len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        if (i + static_cast<std::size_t>(len) > s.size()) { cp = U'?'; len = 1; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        i += static_cast<std::size_t>(len);
        if (cp == U'\r') continue;
        if (cp == U'\n') { lines.emplace_back(); continue; }
        lines.back().push_back(cp == U'\t' ? U' ' : cp);
    }
    return lines;
}

} // namespace

Font* load(const std::string& path) {
    static std::map<std::string, std::unique_ptr<Font>> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second.get();
    auto f = std::make_unique<Font>();
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (in) f->bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const int offset = f->bytes.empty() ? -1 : stbtt_GetFontOffsetForIndex(f->bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, f->bytes.data(), offset)) f.reset();
    if (f) {
        stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->lineGap);
        int x0, y0, x1, y1;
        f->capUnits = stbtt_GetCodepointBox(&f->info, 'H', &x0, &y0, &x1, &y1) && y1 > 0
                          ? static_cast<double>(y1)
                          : 0.7 * f->ascent;
        // A thousandth of the em: a hundredth of a millimetre on a 10 mm letter.
        f->tolerance = 0.001 / static_cast<double>(stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f));
    }
    return cache.emplace(path, std::move(f)).first->second.get();
}

std::string defaultFontPath() {
    static const char* candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return {};
}

const char* alignName(Align a) {
    switch (a) {
    case Align::Centre: return "Centre";
    case Align::Right:  return "Right";
    default:            return "Left";
    }
}

std::vector<std::vector<polyfill::DV>> outlinesOf(Font& f, char32_t c) {
    const int g = stbtt_FindGlyphIndex(&f.info, static_cast<int>(c));
    return g ? outlinesOf(static_cast<const Font&>(f), g) : std::vector<std::vector<polyfill::DV>>{};
}

const Shape* shapeOf(Font& f, char32_t c) {
    const int g = stbtt_FindGlyphIndex(&f.info, static_cast<int>(c));
    return g ? shapeFor(f, g) : nullptr;
}

Block layout(Font& f, const std::string& utf8, const Style& st) {
    Block b;
    const std::vector<std::u32string> lines = linesOf(utf8);
    struct Line { std::vector<Placed> glyphs; double width = 0.0; double baseline = 0.0; };
    std::vector<Line> out;
    double prevScale = 0.0, baseline = 0.0;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        const double s = st.capHeight * (k == 0 ? st.headline : 1.0f) / f.capUnits;
        baseline = k == 0 ? -f.capUnits * s
                          : baseline - st.spacing * ((f.ascent + f.lineGap) * s - f.descent * prevScale);
        prevScale = s;
        Line l;
        l.baseline = baseline;
        double pen = 0.0;
        int prev = 0;
        for (char32_t c : lines[k]) {
            const int g = stbtt_FindGlyphIndex(&f.info, static_cast<int>(c));
            if (!g) { ++b.missing; continue; }
            if (prev) pen += stbtt_GetGlyphKernAdvance(&f.info, prev, g) * s;
            const Shape* sh = shapeFor(f, g);
            if (!sh->fill.tris.empty() || !sh->traps.empty()) {
                l.glyphs.push_back({sh, s, glm::dvec2(pen, baseline)});
                b.fellBack += sh->fellBack ? 1 : 0;
            }
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&f.info, g, &adv, &lsb);
            pen += adv * s;
            prev = g;
        }
        l.width = pen;
        b.width = std::max(b.width, pen);
        out.push_back(std::move(l));
    }
    for (Line& l : out) {
        const double shift = st.align == Align::Centre ? 0.5 * (b.width - l.width)
                           : st.align == Align::Right  ? b.width - l.width
                                                       : 0.0;
        for (Placed& p : l.glyphs) {
            p.at.x += shift;
            b.glyphs.push_back(p);
        }
    }
    b.height  = -baseline;
    b.descent = -f.descent * prevScale;
    return b;
}

} // namespace lettering
