// The image editor's tools: filters and adjustments, brush strokes and shapes,
// gradients, text, colour helpers. The document they write into is ImageDoc.cpp.
#include "ImageDoc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include "ImagePixel.hpp"

namespace fs = std::filesystem;

namespace img {

// --- Colour helpers -------------------------------------------------------------------

void toHsv(const Color& c, float& h, float& s, float& v) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    const float d = mx - mn;
    v = mx;
    s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) { h = 0.0f; return; }
    if (mx == c.r)      h = std::fmod((c.g - c.b) / d, 6.0f);
    else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
    else                h = (c.r - c.g) / d + 4.0f;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
}

Color fromHsv(float h, float s, float v, float a) {
    h = (h - std::floor(h)) * 6.0f;
    const int i = int(h) % 6;
    const float f = h - std::floor(h);
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i) {
    case 0:  return {v, t, p, a};
    case 1:  return {q, v, p, a};
    case 2:  return {p, v, t, a};
    case 3:  return {p, q, v, a};
    case 4:  return {t, p, v, a};
    default: return {v, p, q, a};
    }
}

std::string hexOf(const Color& c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", px::to8(c.r), px::to8(c.g), px::to8(c.b));
    return buf;
}

// --- Filters --------------------------------------------------------------------------

namespace {

const FilterInfo kFilters[] = {
    {"Brightness / Contrast", "Adjust", 2, {"Brightness", "Contrast", ""},
     {-100, -100, 0}, {100, 100, 0}, {0, 0, 0}, {5, 5, 0}, {"%+.0f", "%+.0f", ""},
     "Lighter or darker; flatter or punchier."},
    {"Hue / Saturation", "Adjust", 3, {"Hue", "Saturation", "Lightness"},
     {-180, -100, -100}, {180, 100, 100}, {0, 0, 0}, {10, 5, 5}, {"%+.0f deg", "%+.0f", "%+.0f"},
     "Turn the colours round the wheel, wash them out or make them stronger."},
    {"Levels", "Adjust", 3, {"Black point", "Gamma", "White point"},
     {0, 0.1f, 1}, {254, 5, 255}, {0, 1, 255}, {5, 0.05f, 5}, {"%.0f", "%.2f", "%.0f"},
     "Stretch the tones: what becomes black, what becomes white, how the middle bends."},
    {"Color balance", "Adjust", 3, {"Cyan - Red", "Magenta - Green", "Yellow - Blue"},
     {-100, -100, -100}, {100, 100, 100}, {0, 0, 0}, {5, 5, 5}, {"%+.0f", "%+.0f", "%+.0f"},
     "Push the mid-tones towards a colour."},
    {"Exposure", "Adjust", 1, {"Stops", "", ""},
     {-4, 0, 0}, {4, 0, 0}, {0, 0, 0}, {0.25f, 0, 0}, {"%+.2f EV", "", ""},
     "As if the photo had been taken with more or less light."},
    {"Blur", "Filter", 1, {"Radius", "", ""},
     {0.5f, 0, 0}, {100, 0, 0}, {3, 0, 0}, {0.5f, 0, 0}, {"%.1f px", "", ""},
     "Gaussian blur."},
    {"Sharpen", "Filter", 3, {"Amount", "Radius", "Threshold"},
     {0, 0.5f, 0}, {500, 20, 50}, {100, 1.5f, 0}, {10, 0.5f, 1}, {"%.0f %%", "%.1f px", "%.0f"},
     "Unsharp mask: edges get more contrast. Threshold leaves flat areas (and their noise) alone."},
    {"Add noise", "Filter", 2, {"Amount", "Colour", ""},
     {0, 0, 0}, {100, 100, 0}, {10, 0, 0}, {2, 10, 0}, {"%.0f %%", "%.0f %%", ""},
     "Grain. Colour 0 % is grey grain, 100 % coloured."},
    {"Pixelate", "Filter", 1, {"Cell", "", ""},
     {2, 0, 0}, {128, 0, 0}, {8, 0, 0}, {1, 0, 0}, {"%.0f px", "", ""},
     "Big square pixels."},
    {"Posterize", "Filter", 1, {"Levels", "", ""},
     {2, 0, 0}, {32, 0, 0}, {4, 0, 0}, {1, 0, 0}, {"%.0f", "", ""},
     "Only so many tones per channel."},
    {"Threshold", "Filter", 1, {"Level", "", ""},
     {0, 0, 0}, {255, 0, 0}, {128, 0, 0}, {5, 0, 0}, {"%.0f", "", ""},
     "Black or white, nothing between."},
    {"Invert", "Adjust", 0, {"", "", ""}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {"", "", ""},
     "Negative."},
    {"Desaturate", "Adjust", 0, {"", "", ""}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {"", "", ""},
     "Grey by luminance."},
    {"Make tileable", "Filter", 1, {"Seam blend", "", ""},
     {5, 0, 0}, {100, 0, 0}, {35, 0, 0}, {5, 0, 0}, {"%.0f %%", "", ""},
     "Cross-fades the edges with the middle so the picture repeats without a seam -- for "
     "textures. Check it with Layer > Offset by half."},
    {"Normal map from height", "Filter", 2, {"Strength", "Smoothing", ""},
     {0.5f, 0, 0}, {40, 10, 0}, {6, 1, 0}, {0.5f, 0.5f, 0}, {"%.1f", "%.1f px", ""},
     "Reads brightness as height and writes a tangent-space normal map (OpenGL / glTF: green up). "
     "Wraps at the edges, so a tileable height map gives a tileable normal map."},
};
static_assert(sizeof(kFilters) / sizeof(kFilters[0]) == std::size_t(Filter::Count));

// Premultiplied float copy and back: blurs and resamples work on this so the
// hidden colour of see-through pixels never bleeds.
std::vector<float> toPremul(const Pixels& p) {
    std::vector<float> f(p.size());
    for (std::size_t i = 0; i < p.size(); i += 4) {
        const float a = p[i + 3] * px::k1_255;
        f[i + 0] = p[i + 0] * px::k1_255 * a;
        f[i + 1] = p[i + 1] * px::k1_255 * a;
        f[i + 2] = p[i + 2] * px::k1_255 * a;
        f[i + 3] = a;
    }
    return f;
}

void fromPremul(const std::vector<float>& f, Pixels& p) {
    p.resize(f.size());
    for (std::size_t i = 0; i < f.size(); i += 4) {
        const float a = f[i + 3];
        const float inv = a > 1e-6f ? 1.0f / a : 0.0f;
        p[i + 0] = px::to8(f[i + 0] * inv);
        p[i + 1] = px::to8(f[i + 1] * inv);
        p[i + 2] = px::to8(f[i + 2] * inv);
        p[i + 3] = px::to8(a);
    }
}

// One box pass of radius r along rows (horiz) or columns, edges clamped,
// `ch` channels per pixel.
void boxPass(std::vector<float>& v, int w, int h, int ch, int r, bool horiz) {
    if (r <= 0) return;
    const int n = horiz ? w : h, lines = horiz ? h : w;
    std::vector<float> line(std::size_t(n) * ch), out(std::size_t(n) * ch);
    const float inv = 1.0f / float(2 * r + 1);
    for (int l = 0; l < lines; ++l) {
        auto at = [&](int k) { return horiz ? (std::size_t(l) * w + k) * ch : (std::size_t(k) * w + l) * ch; };
        for (int k = 0; k < n; ++k) std::memcpy(&line[std::size_t(k) * ch], &v[at(k)], sizeof(float) * ch);
        float acc[4] = {0, 0, 0, 0};
        for (int j = -r; j <= r; ++j) {
            const int k = std::clamp(j, 0, n - 1);
            for (int c = 0; c < ch; ++c) acc[c] += line[std::size_t(k) * ch + c];
        }
        for (int k = 0; k < n; ++k) {
            for (int c = 0; c < ch; ++c) out[std::size_t(k) * ch + c] = acc[c] * inv;
            const int add = std::min(k + r + 1, n - 1), sub = std::max(k - r, 0);
            for (int c = 0; c < ch; ++c) acc[c] += line[std::size_t(add) * ch + c] - line[std::size_t(sub) * ch + c];
        }
        for (int k = 0; k < n; ++k) std::memcpy(&v[at(k)], &out[std::size_t(k) * ch], sizeof(float) * ch);
    }
}

// Gaussian of standard deviation sigma as three box blurs.
void gauss(std::vector<float>& v, int w, int h, int ch, float sigma) {
    if (sigma < 0.3f) return;
    const int passes = 3;
    const float ideal = std::sqrt(12.0f * sigma * sigma / passes + 1.0f);
    int wl = int(std::floor(ideal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const float mIdeal = (12.0f * sigma * sigma - passes * wl * wl - 4.0f * passes * wl - 3.0f * passes) /
                         (-4.0f * wl - 4.0f);
    const int m = int(std::lround(mIdeal));
    for (int i = 0; i < passes; ++i) {
        const int r = ((i < m ? wl : wu) - 1) / 2;
        boxPass(v, w, h, ch, r, true);
        boxPass(v, w, h, ch, r, false);
    }
}

void rgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d <= 1e-6f) { h = 0.0f; s = 0.0f; return; }
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else              h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

float hue2rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s <= 0.0f) { r = g = b = l; return; }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    r = hue2rgb(p, q, h + 1.0f / 3.0f);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.0f / 3.0f);
}

float luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

template <class Fn>
void mapRgb(Pixels& p, Fn fn) {
    for (std::size_t i = 0; i < p.size(); i += 4) {
        float r = p[i] * px::k1_255, g = p[i + 1] * px::k1_255, b = p[i + 2] * px::k1_255;
        fn(r, g, b);
        p[i] = px::to8(r); p[i + 1] = px::to8(g); p[i + 2] = px::to8(b);
    }
}

struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed * 2654435761u + 1u) {}
    float next() {  // 0..1
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (1.0f / 16777216.0f);
    }
};

float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

const FilterInfo& filterInfo(Filter f) { return kFilters[std::size_t(f)]; }

void applyFilter(Filter f, const float* prm, const Pixels& src, Pixels& dst, int w, int h,
                 const Mask* sel, std::uint32_t seed) {
    const float p0 = prm ? prm[0] : 0.0f, p1 = prm ? prm[1] : 0.0f, p2 = prm ? prm[2] : 0.0f;
    Pixels out = src;
    const std::size_t n = std::size_t(w) * h;
    switch (f) {
    case Filter::BrightnessContrast: {
        const float b = p0 / 100.0f * 0.5f, c = p1 / 100.0f;
        const float k = c >= 0.0f ? 1.0f / std::max(0.01f, 1.0f - c * 0.99f) : 1.0f + c;
        mapRgb(out, [&](float& r, float& g, float& bl) {
            r = (r - 0.5f) * k + 0.5f + b; g = (g - 0.5f) * k + 0.5f + b; bl = (bl - 0.5f) * k + 0.5f + b;
        });
        break;
    }
    case Filter::HueSaturation: {
        const float dh = p0 / 360.0f, ds = p1 / 100.0f, dl = p2 / 100.0f;
        mapRgb(out, [&](float& r, float& g, float& b) {
            float hh, s, l;
            rgbToHsl(r, g, b, hh, s, l);
            hh += dh; hh -= std::floor(hh);
            s = ds >= 0.0f ? s + (1.0f - s) * ds * s : s * (1.0f + ds);  // grey stays grey
            s = std::clamp(s, 0.0f, 1.0f);
            hslToRgb(hh, s, l, r, g, b);
            if (dl > 0.0f) { r += (1.0f - r) * dl; g += (1.0f - g) * dl; b += (1.0f - b) * dl; }
            else if (dl < 0.0f) { r *= 1.0f + dl; g *= 1.0f + dl; b *= 1.0f + dl; }
        });
        break;
    }
    case Filter::Levels: {
        const float lo = p0 / 255.0f, hi = std::max(lo + 1.0f / 255.0f, p2 / 255.0f);
        const float ig = 1.0f / std::max(0.05f, p1);
        auto lv = [&](float v) { return std::pow(std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f), ig); };
        mapRgb(out, [&](float& r, float& g, float& b) { r = lv(r); g = lv(g); b = lv(b); });
        break;
    }
    case Filter::ColorBalance: {
        const float cr = p0 / 100.0f * 0.4f, cg = p1 / 100.0f * 0.4f, cb = p2 / 100.0f * 0.4f;
        mapRgb(out, [&](float& r, float& g, float& b) {
            const float l = luma(r, g, b);
            const float mid = 1.0f - std::fabs(2.0f * l - 1.0f);   // mostly the mid-tones
            r += cr * mid; g += cg * mid; b += cb * mid;
        });
        break;
    }
    case Filter::Exposure: {
        const float k = std::exp2(p0);
        auto ex = [&](float v) { return std::pow(std::pow(v, 2.2f) * k, 1.0f / 2.2f); };
        mapRgb(out, [&](float& r, float& g, float& b) { r = ex(r); g = ex(g); b = ex(b); });
        break;
    }
    case Filter::Blur: {
        std::vector<float> v = toPremul(src);
        gauss(v, w, h, 4, p0 * 0.5f + 0.25f);   // the radius reads as about two sigma
        fromPremul(v, out);
        break;
    }
    case Filter::Sharpen: {
        std::vector<float> v = toPremul(src);
        gauss(v, w, h, 4, p1);
        Pixels blurred;
        fromPremul(v, blurred);
        const float amt = p0 / 100.0f, thr = p2 / 255.0f;
        for (std::size_t i = 0; i < out.size(); i += 4)
            for (int c = 0; c < 3; ++c) {
                const float o = src[i + c] * px::k1_255, d = o - blurred[i + c] * px::k1_255;
                if (std::fabs(d) >= thr) out[i + c] = px::to8(o + d * amt);
            }
        break;
    }
    case Filter::Noise: {
        Rng rng(seed);
        const float amt = p0 / 100.0f, col = p1 / 100.0f;
        for (std::size_t i = 0; i < out.size(); i += 4) {
            const float mono = rng.next() - 0.5f;
            for (int c = 0; c < 3; ++c) {
                const float nz = mono * (1.0f - col) + (rng.next() - 0.5f) * col;
                out[i + c] = px::to8(src[i + c] * px::k1_255 + nz * amt);
            }
        }
        break;
    }
    case Filter::Pixelate: {
        const int cell = std::max(2, int(p0));
        for (int cy = 0; cy < h; cy += cell)
            for (int cx = 0; cx < w; cx += cell) {
                const int x1 = std::min(w, cx + cell), y1 = std::min(h, cy + cell);
                double acc[4] = {0, 0, 0, 0};
                for (int y = cy; y < y1; ++y)
                    for (int x = cx; x < x1; ++x) {
                        const std::uint8_t* q = &src[(std::size_t(y) * w + x) * 4];
                        const double a = q[3] / 255.0;
                        acc[0] += q[0] * a; acc[1] += q[1] * a; acc[2] += q[2] * a; acc[3] += a;
                    }
                const double cnt = double(x1 - cx) * (y1 - cy);
                std::uint8_t c[4];
                for (int k = 0; k < 3; ++k) c[k] = std::uint8_t(acc[3] > 0 ? std::clamp(acc[k] / acc[3], 0.0, 255.0) + 0.5 : 0);
                c[3] = px::to8(float(acc[3] / cnt));
                for (int y = cy; y < y1; ++y)
                    for (int x = cx; x < x1; ++x) std::memcpy(&out[(std::size_t(y) * w + x) * 4], c, 4);
            }
        break;
    }
    case Filter::Posterize: {
        const float lv = std::max(2.0f, std::round(p0)) - 1.0f;
        mapRgb(out, [&](float& r, float& g, float& b) {
            r = std::round(r * lv) / lv; g = std::round(g * lv) / lv; b = std::round(b * lv) / lv;
        });
        break;
    }
    case Filter::Threshold: {
        const float t = p0 / 255.0f;
        mapRgb(out, [&](float& r, float& g, float& b) { r = g = b = luma(r, g, b) >= t ? 1.0f : 0.0f; });
        break;
    }
    case Filter::Invert:
        mapRgb(out, [](float& r, float& g, float& b) { r = 1.0f - r; g = 1.0f - g; b = 1.0f - b; });
        break;
    case Filter::Desaturate:
        mapRgb(out, [](float& r, float& g, float& b) { r = g = b = luma(r, g, b); });
        break;
    case Filter::Tileable: {
        // Four copies -- as is, shifted half across, half down, both -- each
        // weighted to nothing where its own seam lies. The edges come from the
        // shifted copies, which are continuous across the wrap.
        const float band = std::clamp(p0 / 100.0f, 0.05f, 1.0f);
        const std::vector<float> v = toPremul(src);
        std::vector<float> o(v.size());
        const int hw = w / 2, hh = h / 2;
        std::vector<float> wx(static_cast<std::size_t>(w)), wy(static_cast<std::size_t>(h));
        for (int x = 0; x < w; ++x) wx[std::size_t(x)] = smooth01(std::min(x + 0.5f, w - x - 0.5f) / (w * 0.5f) / band);
        for (int y = 0; y < h; ++y) wy[std::size_t(y)] = smooth01(std::min(y + 0.5f, h - y - 0.5f) / (h * 0.5f) / band);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float ax = wx[std::size_t(x)], ay = wy[std::size_t(y)];
                const int xs[2] = {x, (x + hw) % w}, ys[2] = {y, (y + hh) % h};
                const float wxs[2] = {ax, 1.0f - ax}, wys[2] = {ay, 1.0f - ay};
                float acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i) {
                        const float k = wxs[i] * wys[j];
                        if (k <= 0.0f) continue;
                        const float* s = &v[(std::size_t(ys[j]) * w + xs[i]) * 4];
                        for (int c = 0; c < 4; ++c) acc[c] += s[c] * k;
                    }
                std::memcpy(&o[(std::size_t(y) * w + x) * 4], acc, sizeof acc);
            }
        fromPremul(o, out);
        break;
    }
    case Filter::NormalMap: {
        std::vector<float> ht(n);
        for (std::size_t i = 0; i < n; ++i)
            ht[i] = luma(src[i * 4] * px::k1_255, src[i * 4 + 1] * px::k1_255, src[i * 4 + 2] * px::k1_255);
        gauss(ht, w, h, 1, p1 * 0.5f);
        const float s = p0;
        auto H = [&](int x, int y) { return ht[std::size_t((y + h) % h) * w + std::size_t((x + w) % w)]; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float gx = (H(x + 1, y) - H(x - 1, y)) * 0.5f;
                const float gu = (H(x, y - 1) - H(x, y + 1)) * 0.5f;   // up the picture
                float nx = -gx * s, ny = -gu * s, nz = 1.0f;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= len; ny /= len; nz /= len;
                std::uint8_t* q = &out[(std::size_t(y) * w + x) * 4];
                q[0] = px::to8(nx * 0.5f + 0.5f); q[1] = px::to8(ny * 0.5f + 0.5f);
                q[2] = px::to8(nz * 0.5f + 0.5f); q[3] = 255;
            }
        break;
    }
    default: break;
    }
    if (sel) {
        for (std::size_t i = 0; i < n; ++i) {
            const int s = (*sel)[i];
            if (s == 255) continue;
            for (int c = 0; c < 4; ++c)
                out[i * 4 + c] = std::uint8_t((src[i * 4 + c] * (255 - s) + out[i * 4 + c] * s + 127) / 255);
        }
    }
    dst = std::move(out);
}

// --- Strokes --------------------------------------------------------------------------

void Stroke::begin(Document& d, PaintMode mode, Color c, const Brush& b, float cdx, float cdy,
                   const char* label) {
    if (m_doc) end();
    m_doc = &d;
    m_mode = mode;
    m_color = c;
    m_brush = b;
    m_brush.radius = std::max(0.5f, b.radius);
    m_cdx = cdx; m_cdy = cdy;
    m_cov.assign(std::size_t(d.width()) * d.height(), 0);
    m_touched = {};
    m_first = true;
    m_carry = 0.0f;
    const char* name = label ? label : mode == PaintMode::Erase ? "Eraser" : mode == PaintMode::Clone ? "Clone stamp" : "Brush";
    d.beginEdit(name);
}

void Stroke::to(float x, float y) {
    if (!m_doc) return;
    if (m_first) {
        dab(x, y);
        m_first = false;
        m_lx = x; m_ly = y;
        return;
    }
    const float dx = x - m_lx, dy = y - m_ly;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    const float step = std::max(0.5f, m_brush.radius * m_brush.spacing);
    float t = step - m_carry;
    while (t <= len) {
        dab(m_lx + dx * t / len, m_ly + dy * t / len);
        t += step;
    }
    m_carry = len - (t - step);
    m_lx = x; m_ly = y;
}

void Stroke::dab(float x, float y) {
    Document& d = *m_doc;
    const float r = m_brush.radius;
    const Rect box = Rect::around(x, y, r).clipped(d.width(), d.height());
    if (box.empty()) return;
    const float inner = r * std::clamp(m_brush.hardness, 0.0f, 1.0f);
    const int w = d.width();
    for (int py = box.y0; py < box.y1; ++py)
        for (int pxl = box.x0; pxl < box.x1; ++pxl) {
            const float ddx = pxl + 0.5f - x, ddy = py + 0.5f - y;
            const float dist = std::sqrt(ddx * ddx + ddy * ddy);
            float cov = std::clamp(r - dist + 0.5f, 0.0f, 1.0f);    // the anti-aliased rim
            if (cov <= 0.0f) continue;
            if (dist > inner) cov = std::min(cov, 1.0f - smooth01((dist - inner) / std::max(1e-3f, r - inner)));
            const std::uint8_t c8 = px::to8(cov);
            std::uint8_t& slot = m_cov[std::size_t(py) * w + pxl];
            if (c8 > slot) slot = c8;
        }
    shade(box);
}

void Stroke::shade(const Rect& rIn) {
    Document& d = *m_doc;
    const Rect r = rIn.clipped(d.width(), d.height());
    if (r.empty()) return;
    const int w = d.width(), h = d.height();
    const Pixels& base = d.editBase();
    std::uint8_t* out = d.pixelsMut(d.active());
    const int ox = int(std::lround(m_cdx)), oy = int(std::lround(m_cdy));
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            const std::size_t i = std::size_t(y) * w + x;
            const int c = m_cov[i];
            if (!c) continue;
            const float a = c * px::k1_255 * m_brush.opacity * d.selAt(i) * px::k1_255;
            std::uint8_t* q = out + i * 4;
            std::memcpy(q, &base[i * 4], 4);
            if (a <= 0.0f) continue;
            switch (m_mode) {
            case PaintMode::Paint:
                px::over(q, m_color.r, m_color.g, m_color.b, a * m_color.a);
                break;
            case PaintMode::Erase:
                q[3] = px::to8(q[3] * px::k1_255 * (1.0f - a));
                break;
            case PaintMode::Clone: {
                const int sx = x + ox, sy = y + oy;
                if (sx < 0 || sy < 0 || sx >= w || sy >= h) break;
                const std::uint8_t* s = &base[(std::size_t(sy) * w + sx) * 4];
                px::over(q, s[0] * px::k1_255, s[1] * px::k1_255, s[2] * px::k1_255, a * s[3] * px::k1_255);
                break;
            }
            }
        }
    d.dirty(r);
    m_touched.add(r);
}

void Stroke::clearCoverage() {
    if (!m_doc || m_touched.empty()) return;
    const int w = m_doc->width();
    for (int y = m_touched.y0; y < m_touched.y1; ++y)
        std::memset(&m_cov[std::size_t(y) * w + m_touched.x0], 0, std::size_t(m_touched.w()));
    m_doc->restoreFromBase(m_touched);
    m_touched = {};
}

void Stroke::shape(Shape s, float x0, float y0, float x1, float y1, float width) {
    if (!m_doc) return;
    clearCoverage();
    Document& d = *m_doc;
    const float hw = std::max(0.5f, width * 0.5f);
    const Rect box = Rect{int(std::floor(std::min(x0, x1) - hw - 2)), int(std::floor(std::min(y0, y1) - hw - 2)),
                          int(std::ceil(std::max(x0, x1) + hw + 2)), int(std::ceil(std::max(y0, y1) + hw + 2))}
                         .clipped(d.width(), d.height());
    if (box.empty()) return;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float ex = std::max(0.5f, std::fabs(x1 - x0) * 0.5f), ey = std::max(0.5f, std::fabs(y1 - y0) * 0.5f);
    const int w = d.width();
    for (int py = box.y0; py < box.y1; ++py)
        for (int pxl = box.x0; pxl < box.x1; ++pxl) {
            const float qx = pxl + 0.5f, qy = py + 0.5f;
            float cov = 0.0f;
            if (s == Shape::Line) {
                const float vx = x1 - x0, vy = y1 - y0;
                const float L2 = vx * vx + vy * vy;
                const float t = L2 > 0.0f ? std::clamp(((qx - x0) * vx + (qy - y0) * vy) / L2, 0.0f, 1.0f) : 0.0f;
                const float dx = qx - (x0 + vx * t), dy = qy - (y0 + vy * t);
                cov = hw - std::sqrt(dx * dx + dy * dy) + 0.5f;
            } else {
                float sd;
                if (s == Shape::Rect || s == Shape::RectFill) {
                    const float ax = std::fabs(qx - cx) - ex, ay = std::fabs(qy - cy) - ey;
                    const float outx = std::max(ax, 0.0f), outy = std::max(ay, 0.0f);
                    sd = std::sqrt(outx * outx + outy * outy) + std::min(std::max(ax, ay), 0.0f);
                } else {
                    const float ux = (qx - cx) / ex, uy = (qy - cy) / ey;
                    const float k0 = std::sqrt(ux * ux + uy * uy);
                    const float vx = (qx - cx) / (ex * ex), vy = (qy - cy) / (ey * ey);
                    const float k1 = std::sqrt(vx * vx + vy * vy);
                    sd = k1 > 1e-6f ? k0 * (k0 - 1.0f) / k1 : -std::min(ex, ey);
                }
                cov = (s == Shape::RectFill || s == Shape::EllipseFill) ? 0.5f - sd : hw - std::fabs(sd) + 0.5f;
            }
            cov = std::clamp(cov, 0.0f, 1.0f);
            if (cov > 0.0f) m_cov[std::size_t(py) * w + pxl] = px::to8(cov);
        }
    shade(box);
}

void Stroke::end() {
    if (!m_doc) return;
    if (m_touched.empty()) m_doc->cancelEdit();
    else m_doc->endEdit();
    m_doc = nullptr;
    m_cov.clear();
    m_cov.shrink_to_fit();
}

void Stroke::cancel() {
    if (!m_doc) return;
    m_doc->cancelEdit();
    m_doc = nullptr;
    m_cov.clear();
    m_cov.shrink_to_fit();
}

void gradient(Document& d, float x0, float y0, float x1, float y1, Color a, Color b, bool radial,
              float opacity) {
    if (!d.editing()) return;
    const int w = d.width(), h = d.height();
    const Pixels& base = d.editBase();
    std::uint8_t* out = d.pixelsMut(d.active());
    const float vx = x1 - x0, vy = y1 - y0;
    const float L2 = std::max(1e-6f, vx * vx + vy * vy), L = std::sqrt(L2);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = std::size_t(y) * w + x;
            std::uint8_t* q = out + i * 4;
            std::memcpy(q, &base[i * 4], 4);
            const int s = d.selAt(i);
            if (!s) continue;
            const float qx = x + 0.5f - x0, qy = y + 0.5f - y0;
            const float t = std::clamp(radial ? std::sqrt(qx * qx + qy * qy) / L : (qx * vx + qy * vy) / L2, 0.0f, 1.0f);
            const float ca = a.a + (b.a - a.a) * t;
            px::over(q, a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                     ca * opacity * s * px::k1_255);
        }
    d.dirtyAll();
}

// --- Text -----------------------------------------------------------------------------

namespace {

#ifdef _WIN32
std::string narrow(const wchar_t* s, int len = -1) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// The installed fonts as Windows lists them: "Arial Bold (TrueType)" -> arialbd.ttf.
void listRegistryFonts(HKEY root, const std::wstring& fontDir, std::vector<FontFile>& out) {
    HKEY key;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        BYTE data[2048];
        DWORD nameLen = 512, dataLen = sizeof data - 2, type = 0;
        const LONG res = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data, &dataLen);
        if (res == ERROR_NO_MORE_ITEMS) break;
        if (res != ERROR_SUCCESS || type != REG_SZ) continue;
        data[dataLen] = 0; data[dataLen + 1] = 0;
        std::wstring file(reinterpret_cast<const wchar_t*>(data));
        std::wstring lower = file;
        for (wchar_t& c : lower) c = wchar_t(towlower(c));
        const bool ok = lower.size() > 4 && (lower.compare(lower.size() - 4, 4, L".ttf") == 0 ||
                                             lower.compare(lower.size() - 4, 4, L".otf") == 0 ||
                                             lower.compare(lower.size() - 4, 4, L".ttc") == 0);
        if (!ok) continue;
        if (file.find(L'\\') == std::wstring::npos && file.find(L'/') == std::wstring::npos) file = fontDir + file;
        std::string nm = narrow(name, int(nameLen));
        const std::size_t paren = nm.rfind(" (");
        if (paren != std::string::npos) nm.resize(paren);
        out.push_back({nm, fs::path(file).generic_string()});
    }
    RegCloseKey(key);
}
#endif

const std::vector<unsigned char>* fontBytes(const std::string& path) {
    static std::map<std::string, std::vector<unsigned char>> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<unsigned char>& b = cache[path];
    std::ifstream f(fs::path(path), std::ios::binary);
    if (f) b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return b.empty() ? nullptr : &b;
}

std::vector<std::vector<int>> splitLines(const std::string& s) {
    std::vector<std::vector<int>> lines(1);
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int cp = c, len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        if (i + std::size_t(len) > s.size()) len = 1, cp = '?';
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + std::size_t(k)]) & 0x3F);
        i += std::size_t(len);
        if (cp == '\r') continue;
        if (cp == '\n') { lines.emplace_back(); continue; }
        lines.back().push_back(cp);
    }
    return lines;
}

} // namespace

const std::vector<FontFile>& systemFonts() {
    static std::vector<FontFile> fonts;
    static bool scanned = false;
    if (scanned) return fonts;
    scanned = true;
#ifdef _WIN32
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring dir = std::wstring(win) + L"\\Fonts\\";
    listRegistryFonts(HKEY_LOCAL_MACHINE, dir, fonts);
    listRegistryFonts(HKEY_CURRENT_USER, dir, fonts);
#else
    for (const char* d : {"/usr/share/fonts", "/System/Library/Fonts"}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(d, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".ttf") fonts.push_back({it->path().stem().string(), it->path().generic_string()});
    }
#endif
    std::sort(fonts.begin(), fonts.end(), [](const FontFile& a, const FontFile& b) { return a.name < b.name; });
    fonts.erase(std::unique(fonts.begin(), fonts.end(), [](const FontFile& a, const FontFile& b) { return a.name == b.name; }),
                fonts.end());
    return fonts;
}

bool renderText(const std::string& fontPath, const std::string& text, float sizePx, Color c,
                Pixels& out, int& w, int& h) {
    const std::vector<unsigned char>* bytes = fontBytes(fontPath);
    if (!bytes) return false;
    stbtt_fontinfo font;
    const int offset = stbtt_GetFontOffsetForIndex(bytes->data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font, bytes->data(), offset)) return false;
    const float scale = stbtt_ScaleForPixelHeight(&font, std::max(4.0f, sizePx));
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    const float lineH = (ascent - descent + gap) * scale;
    const auto lines = splitLines(text);
    float maxW = 0.0f;
    for (const auto& l : lines) {
        float x = 0.0f;
        for (std::size_t k = 0; k < l.size(); ++k) {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&font, l[k], &adv, &lsb);
            x += adv * scale;
            if (k + 1 < l.size()) x += stbtt_GetCodepointKernAdvance(&font, l[k], l[k + 1]) * scale;
        }
        maxW = std::max(maxW, x);
    }
    const int pad = int(std::ceil(sizePx * 0.15f)) + 2;
    w = std::max(1, int(std::ceil(maxW)) + pad * 2);
    h = std::max(1, int(std::ceil(lineH * float(lines.size()))) + pad * 2);
    std::vector<unsigned char> cov(std::size_t(w) * h, 0), glyph;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const auto& l = lines[li];
        const float baseline = pad + ascent * scale + lineH * float(li);
        float x = float(pad);
        for (std::size_t k = 0; k < l.size(); ++k) {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&font, l[k], &adv, &lsb);
            const float fx = x - std::floor(x), fy = baseline - std::floor(baseline);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBoxSubpixel(&font, l[k], scale, scale, fx, fy, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                glyph.assign(std::size_t(gw) * gh, 0);
                stbtt_MakeCodepointBitmapSubpixel(&font, glyph.data(), gw, gh, gw, scale, scale, fx, fy, l[k]);
                const int ox = int(std::floor(x)) + x0, oy = int(std::floor(baseline)) + y0;
                for (int gy = 0; gy < gh; ++gy)
                    for (int gx = 0; gx < gw; ++gx) {
                        const int tx = ox + gx, ty = oy + gy;
                        if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
                        unsigned char& slot = cov[std::size_t(ty) * w + tx];
                        slot = std::max(slot, glyph[std::size_t(gy) * gw + gx]);
                    }
            }
            x += adv * scale;
            if (k + 1 < l.size()) x += stbtt_GetCodepointKernAdvance(&font, l[k], l[k + 1]) * scale;
        }
    }
    out.assign(std::size_t(w) * h * 4, 0);
    const std::uint8_t r8 = px::to8(c.r), g8 = px::to8(c.g), b8 = px::to8(c.b);
    for (std::size_t i = 0; i < cov.size(); ++i) {
        out[i * 4 + 0] = r8; out[i * 4 + 1] = g8; out[i * 4 + 2] = b8;
        out[i * 4 + 3] = px::to8(cov[i] * px::k1_255 * c.a);
    }
    return true;
}

} // namespace img
