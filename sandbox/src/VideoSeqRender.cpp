#include "VideoSeqRender.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

#include <stb_image_write.h>

#include <fitzel/audio/SampleData.hpp>
#include <fitzel/graphics/VideoTexture.hpp>

#include "ImageDoc.hpp"   // img::renderText, img::systemFonts

namespace vseq {

namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265358979f;

// Rows [0, rows) split over a few threads; small jobs stay on this one.
void parallelRows(int rows, std::int64_t work, const std::function<void(int, int)>& f) {
    static const int cores = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
    const int n = work < 150000 ? 1 : std::min(cores, std::max(1, rows / 16));
    if (n <= 1) {
        f(0, rows);
        return;
    }
    std::vector<std::thread> ts;
    const int per = (rows + n - 1) / n;
    for (int i = 1; i < n; ++i) {
        const int a = i * per, b = std::min(rows, a + per);
        if (a < b) ts.emplace_back([&f, a, b] { f(a, b); });
    }
    f(0, std::min(rows, per));
    for (std::thread& t : ts) t.join();
}

float fadeAt(const Clip& c, double t) {
    float g = 1.0f;
    const double into = t - c.start, left = c.end() - t;
    if (c.fadeIn > 0.0f && into < c.fadeIn) g *= static_cast<float>(std::clamp(into / c.fadeIn, 0.0, 1.0));
    if (c.fadeOut > 0.0f && left < c.fadeOut) g *= static_cast<float>(std::clamp(left / c.fadeOut, 0.0, 1.0));
    return g;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace

// --- Layers -----------------------------------------------------------------------------

void fitSize(Fit fit, int sw, int sh, int w, int h, float& dw, float& dh) {
    sw = std::max(1, sw);
    sh = std::max(1, sh);
    if (fit == Fit::Stretch) {
        dw = static_cast<float>(w);
        dh = static_cast<float>(h);
        return;
    }
    const float kx = static_cast<float>(w) / sw, ky = static_cast<float>(h) / sh;
    const float k = fit == Fit::Fill ? std::max(kx, ky) : std::min(kx, ky);
    dw = sw * k;
    dh = sh * k;
}

void drawLayer(Image& dst, const Image& src, float cx, float cy, float dw, float dh, float degrees, float alpha) {
    if (src.empty() || dst.empty() || dw < 0.25f || dh < 0.25f || alpha <= 0.0f) return;
    alpha = std::min(alpha, 1.0f);
    const float rad = degrees * kPi / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    const float hw = dw * 0.5f, hh = dh * 0.5f;
    // The turned rectangle's bounds on the canvas.
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const auto& [ox, oy] : {std::pair{-hw, -hh}, std::pair{hw, -hh}, std::pair{hw, hh}, std::pair{-hw, hh}}) {
        const float x = cx + ox * cs - oy * sn, y = cy + ox * sn + oy * cs;
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0)) - 1), ix1 = std::min(dst.w, static_cast<int>(std::ceil(x1)) + 1);
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0)) - 1), iy1 = std::min(dst.h, static_cast<int>(std::ceil(y1)) + 1);
    if (ix0 >= ix1 || iy0 >= iy1) return;
    const float ksx = src.w / dw, ksy = src.h / dh;   // source pixels per canvas pixel
    const int sw = src.w, sh = src.h;
    const std::uint8_t* sp = src.px.data();
    std::uint8_t* dp = dst.px.data();
    const int dstW = dst.w;
    const bool axis = std::abs(sn) < 1e-6f && cs > 0.0f;
    parallelRows(iy1 - iy0, static_cast<std::int64_t>(ix1 - ix0) * (iy1 - iy0), [&](int r0, int r1) {
        for (int y = iy0 + r0; y < iy0 + r1; ++y) {
            const float dy = y + 0.5f - cy;
            for (int x = ix0; x < ix1; ++x) {
                const float dx = x + 0.5f - cx;
                // Back into the layer's own frame...
                const float lx = axis ? dx : dx * cs + dy * sn;
                const float ly = axis ? dy : -dx * sn + dy * cs;
                // ...where the edges are antialiased in canvas pixels, whatever
                // the layer's own resolution (a 4x4 colour stretched to the
                // frame keeps a sharp edge).
                const float ex = std::clamp(hw - std::abs(lx) + 0.5f, 0.0f, 1.0f);
                const float ey = std::clamp(hh - std::abs(ly) + 0.5f, 0.0f, 1.0f);
                const float cov = ex * ey * alpha;
                if (cov <= 0.0f) continue;
                float u = (lx + hw) * ksx - 0.5f, v = (ly + hh) * ksy - 0.5f;
                u = std::clamp(u, 0.0f, static_cast<float>(sw - 1));
                v = std::clamp(v, 0.0f, static_cast<float>(sh - 1));
                const int u0 = static_cast<int>(u), v0 = static_cast<int>(v);
                const int u1 = std::min(u0 + 1, sw - 1), v1 = std::min(v0 + 1, sh - 1);
                const float fu = u - u0, fv = v - v0;
                const std::uint8_t* p00 = sp + (static_cast<std::size_t>(v0) * sw + u0) * 4;
                const std::uint8_t* p10 = sp + (static_cast<std::size_t>(v0) * sw + u1) * 4;
                const std::uint8_t* p01 = sp + (static_cast<std::size_t>(v1) * sw + u0) * 4;
                const std::uint8_t* p11 = sp + (static_cast<std::size_t>(v1) * sw + u1) * 4;
                const float w00 = (1 - fu) * (1 - fv) * p00[3], w10 = fu * (1 - fv) * p10[3];
                const float w01 = (1 - fu) * fv * p01[3], w11 = fu * fv * p11[3];
                const float a = (w00 + w10 + w01 + w11) * (1.0f / 255.0f);   // 0..1
                if (a <= 0.0f) continue;
                const float k = cov / 255.0f;   // premultiplied colours, /255 for alpha
                const float oa = a * cov;
                std::uint8_t* d = dp + (static_cast<std::size_t>(y) * dstW + x) * 4;
                for (int c = 0; c < 3; ++c) {
                    const float s = (p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11) * k;
                    const float out = s + d[c] * (1.0f - oa);
                    d[c] = static_cast<std::uint8_t>(std::clamp(out + 0.5f, 0.0f, 255.0f));
                }
                d[3] = 255;
            }
        }
    });
}

// --- Titles ------------------------------------------------------------------------------

std::string defaultFont() {
    static std::string found = [] {
        const auto& fonts = img::systemFonts();
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        for (const char* want : {"dejavusans-bold", "liberationsans-bold", "arial bold", "arialbd", "segoe ui bold",
                                 "seguisb", "roboto-bold", "notosans-bold", "ubuntu-b", "cantarell-bold"})
            for (const auto& f : fonts)
                if (lower(f.name) == want || lower(fs::path(f.path).stem().string()) == want) return f.path;
        for (const auto& f : fonts)
            if (lower(f.name).find("sans") != std::string::npos) return f.path;
        return fonts.empty() ? std::string() : fonts.front().path;
    }();
    return found;
}

namespace {

// Grow a coverage mask by `r` pixels (3x3 max, r times: an octagon).
void dilate(std::vector<std::uint8_t>& a, int w, int h, int r) {
    std::vector<std::uint8_t> b(a.size());
    for (int it = 0; it < r; ++it) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t m = 0;
                for (int yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1); ++yy)
                    for (int xx = std::max(0, x - 1); xx <= std::min(w - 1, x + 1); ++xx)
                        m = std::max(m, a[static_cast<std::size_t>(yy) * w + xx]);
                b[static_cast<std::size_t>(y) * w + x] = m;
            }
        a.swap(b);
    }
}

void boxBlur(std::vector<std::uint8_t>& a, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<std::uint8_t> b(a.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            int sum = 0;
            for (int x = -r; x <= r; ++x) sum += a[static_cast<std::size_t>(y) * w + std::clamp(x, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                b[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(sum / (2 * r + 1));
                sum += a[static_cast<std::size_t>(y) * w + std::min(w - 1, x + r + 1)] -
                       a[static_cast<std::size_t>(y) * w + std::max(0, x - r)];
            }
        }
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int y = -r; y <= r; ++y) sum += b[static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * w + x];
            for (int y = 0; y < h; ++y) {
                a[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(sum / (2 * r + 1));
                sum += b[static_cast<std::size_t>(std::min(h - 1, y + r + 1)) * w + x] -
                       b[static_cast<std::size_t>(std::max(0, y - r)) * w + x];
            }
        }
    }
}

// `cov` (0..255) in colour `c`, over what `out` has (straight alpha).
void over(Image& out, const std::vector<std::uint8_t>& cov, const Rgba& c, int ox = 0, int oy = 0) {
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const int sx = x - ox, sy = y - oy;
            if (sx < 0 || sy < 0 || sx >= out.w || sy >= out.h) continue;
            const float a = cov[static_cast<std::size_t>(sy) * out.w + sx] / 255.0f * std::clamp(c.a, 0.0f, 1.0f);
            if (a <= 0.0f) continue;
            std::uint8_t* d = &out.px[(static_cast<std::size_t>(y) * out.w + x) * 4];
            const float da = d[3] / 255.0f;
            const float oa = a + da * (1.0f - a);
            const float col[3] = {c.r, c.g, c.b};
            for (int k = 0; k < 3; ++k) {
                const float v = (col[k] * 255.0f * a + d[k] * da * (1.0f - a)) / std::max(oa, 1e-6f);
                d[k] = static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
            d[3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
        }
}

std::mutex g_textMx;
std::map<std::string, ImagePtr> g_texts;
std::vector<std::string> g_textOrder;   // oldest first

} // namespace

ImagePtr textImage(const TextStyle& s, int canvasH) {
    char head[512];
    std::snprintf(head, sizeof head, "%d|%.5f|%.4f,%.4f,%.4f,%.4f|%d|%d|%.4f|%.4f,%.4f,%.4f,%.4f|%d|%.4f|%.4f,%.4f,%.4f,%.4f|",
                  canvasH, s.size, s.color.r, s.color.g, s.color.b, s.color.a, s.align, s.bold ? 1 : 0, s.outline,
                  s.outlineColor.r, s.outlineColor.g, s.outlineColor.b, s.outlineColor.a, s.shadow ? 1 : 0, s.box,
                  s.boxColor.r, s.boxColor.g, s.boxColor.b, s.boxColor.a);
    const std::string key = head + s.font + "|" + s.text;
    {
        std::lock_guard<std::mutex> lk(g_textMx);
        if (auto it = g_texts.find(key); it != g_texts.end()) return it->second;
    }
    std::error_code ec;
    const std::string font = !s.font.empty() && fs::exists(s.font, ec) ? s.font : defaultFont();
    const float px = std::max(4.0f, s.size * canvasH);
    // Line by line, so the lines can be set left, centred or right among
    // each other (renderText sets a block flush left).
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char ch : s.text) {
            if (ch == '\n') { lines.push_back(cur); cur.clear(); }
            else if (ch != '\r') cur.push_back(ch);
        }
        lines.push_back(cur);
    }
    const int pad = static_cast<int>(std::ceil(px * 0.15f)) + 2;   // renderText's own
    struct Line { img::Pixels px; int w = 0, h = 0; };
    std::vector<Line> ls(lines.size());
    int contentW = 1, lineH = 0;
    bool any = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (img::renderText(font, lines[i].empty() ? std::string(" ") : lines[i], px, img::Color{1, 1, 1, 1}, ls[i].px,
                            ls[i].w, ls[i].h)) {
            any = true;
            contentW = std::max(contentW, ls[i].w - 2 * pad);
            lineH = std::max(lineH, ls[i].h - 2 * pad);
        }
    }
    if (!any || lineH <= 0) {
        std::lock_guard<std::mutex> lk(g_textMx);
        g_texts[key] = nullptr;
        return nullptr;
    }
    const int ro = static_cast<int>(std::lround(s.outline * px));
    const int rb = s.bold ? std::max(1, static_cast<int>(std::lround(px * 0.035f))) : 0;
    const int shOff = s.shadow ? std::max(1, static_cast<int>(std::lround(px * 0.05f))) : 0;
    const int shBlur = s.shadow ? std::max(1, static_cast<int>(std::lround(px * 0.035f))) : 0;
    const int boxPad = s.box > 0.0f ? static_cast<int>(std::lround(px * 0.3f)) : 0;
    const int margin = std::max(boxPad, ro + rb + shOff + shBlur * 2) + 2;
    auto img = std::make_shared<Image>();
    img->w = contentW + 2 * (pad + margin);
    img->h = lineH * static_cast<int>(lines.size()) + 2 * (pad + margin);
    img->px.assign(static_cast<std::size_t>(img->w) * img->h * 4, 0);
    std::vector<std::uint8_t> cov(static_cast<std::size_t>(img->w) * img->h, 0);
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const Line& l = ls[i];
        if (l.w <= 0) continue;
        const int lw = l.w - 2 * pad;
        const int ox = margin + (s.align == 0 ? 0 : s.align == 1 ? (contentW - lw) / 2 : contentW - lw);
        const int oy = margin + static_cast<int>(i) * lineH;
        for (int y = 0; y < l.h; ++y)
            for (int x = 0; x < l.w; ++x) {
                const int tx = ox + x, ty = oy + y;
                if (tx < 0 || ty < 0 || tx >= img->w || ty >= img->h) continue;
                std::uint8_t& c = cov[static_cast<std::size_t>(ty) * img->w + tx];
                c = std::max(c, l.px[(static_cast<std::size_t>(y) * l.w + x) * 4 + 3]);
            }
    }
    if (rb > 0) dilate(cov, img->w, img->h, rb);
    std::vector<std::uint8_t> outline;
    if (ro > 0) {
        outline = cov;
        dilate(outline, img->w, img->h, ro);
    }
    if (s.box > 0.0f) {
        // The plate: the words' block and a margin round it.
        std::vector<std::uint8_t> plate(cov.size(), 0);
        const int bx0 = margin + pad - boxPad, bx1 = margin + pad + contentW + boxPad;
        const int by0 = margin + pad - boxPad, by1 = margin + pad + lineH * static_cast<int>(lines.size()) + boxPad;
        for (int y = std::max(0, by0); y < std::min(img->h, by1); ++y)
            for (int x = std::max(0, bx0); x < std::min(img->w, bx1); ++x) plate[static_cast<std::size_t>(y) * img->w + x] = 255;
        Rgba bc = s.boxColor;
        bc.a *= s.box;
        over(*img, plate, bc);
    }
    if (s.shadow) {
        std::vector<std::uint8_t> sh = ro > 0 ? outline : cov;
        boxBlur(sh, img->w, img->h, shBlur);
        over(*img, sh, Rgba{0, 0, 0, 0.6f * s.color.a}, shOff, shOff);
    }
    if (ro > 0) over(*img, outline, s.outlineColor);
    over(*img, cov, s.color);
    std::lock_guard<std::mutex> lk(g_textMx);
    g_texts[key] = img;
    g_textOrder.push_back(key);
    while (g_textOrder.size() > 48) {
        g_texts.erase(g_textOrder.front());
        g_textOrder.erase(g_textOrder.begin());
    }
    return img;
}

// --- Frames -------------------------------------------------------------------------------

FrameSource* Frames::source(const Clip& c, const MediaInfo& mi, double fps, int canvasW, int canvasH,
                            std::int64_t& index, double t) {
    float dw = 0, dh = 0;
    fitSize(c.fit, mi.width, mi.height, canvasW, canvasH, dw, dh);
    // Decoded at the size it is drawn (never above the file's own), in steps of
    // a square root of two: a scale being dragged does not restart ffmpeg at
    // every pixel of the drag.
    const float s = std::max(std::abs(c.look.scale), c.animate ? std::abs(c.lookEnd.scale) : 0.0f);
    float q = 1.0f;
    while (q < s && q < 16.0f) q *= 1.41421356f;
    const double k = std::min(1.0, static_cast<double>(std::max(dw * q / std::max(1, mi.width), dh * q / std::max(1, mi.height))));
    const int w = std::max(2, static_cast<int>(std::lround(mi.width * k)) & ~1);
    const int h = std::max(2, static_cast<int>(std::lround(mi.height * k)) & ~1);
    const double rate = fps / std::max(0.05, c.speed);
    const double last = std::max(0.0, mi.duration - 1.0 / rate);
    const double src = std::clamp(c.sourceAt(t), 0.0, last);
    index = std::llround(src * rate);
    char key[64];
    std::snprintf(key, sizeof key, "%d|%d|%d|%lld|", c.uid, w, h, static_cast<long long>(std::llround(rate * 1000.0)));
    const std::string k2 = key + c.file;
    auto& slot = m_src[k2];
    if (!slot) slot = std::make_unique<FrameSource>(m_lib.resolve(c.file), w, h, rate);
    slot->lastUsed = clock;
    return slot.get();
}

ImagePtr Frames::video(const Clip& c, const MediaInfo& mi, double t, double fps, int canvasW, int canvasH, int waitMs,
                       bool* exact, std::string* error) {
    std::int64_t idx = 0;
    FrameSource* fsrc = source(c, mi, fps, canvasW, canvasH, idx, t);
    ImagePtr f = fsrc->get(idx, waitMs);
    if (exact) *exact = f != nullptr;
    if (!f) {
        // Failed for good: nothing is missing that waiting would bring.
        const std::string why = fsrc->error();
        if (!why.empty()) {
            if (exact) *exact = true;
            if (error) *error = why;
            return nullptr;
        }
    }
    if (!f) {
        f = fsrc->latest();
        // Nothing of this clip yet: another clip's source of the same file
        // (the half before a split) may have the frame -- better than black.
        if (!f)
            for (auto& [k, s] : m_src)
                if (s.get() != fsrc && s->path() == fsrc->path())
                    if (ImagePtr o = s->latest()) { f = o; break; }
    }
    return f;
}

void Frames::prefetch(const Clip& c, const MediaInfo& mi, double t, double fps, int canvasW, int canvasH) {
    std::int64_t idx = 0;
    source(c, mi, fps, canvasW, canvasH, idx, t)->prefetch(idx);
}

void Frames::drop(double now, double idle, const std::vector<int>& alive) {
    for (auto it = m_src.begin(); it != m_src.end();) {
        const int uid = std::atoi(it->first.c_str());
        const bool gone = std::find(alive.begin(), alive.end(), uid) == alive.end();
        if (gone || now - it->second->lastUsed > idle) it = m_src.erase(it);
        else ++it;
    }
}

// --- A frame -------------------------------------------------------------------------------

void drawFrame(const Film& film, double t, int w, int h, Library& lib, Frames& frames, int waitMs, Image& out,
               DrawReport* report) {
    w = std::max(2, w);
    h = std::max(2, h);
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<std::size_t>(w) * h * 4);
    const std::uint8_t bg[4] = {static_cast<std::uint8_t>(std::clamp(film.background.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                                static_cast<std::uint8_t>(std::clamp(film.background.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                                static_cast<std::uint8_t>(std::clamp(film.background.b, 0.0f, 1.0f) * 255.0f + 0.5f), 255};
    for (std::size_t i = 0; i < out.px.size(); i += 4) std::memcpy(&out.px[i], bg, 4);
    auto problem = [&](const Clip& c, const std::string& why) {
        if (!report) return;
        const std::string p = (c.name.empty() ? fs::path(c.file).filename().string() : c.name) + ": " + why;
        if (std::find(report->problems.begin(), report->problems.end(), p) == report->problems.end())
            report->problems.push_back(p);
    };
    // V1 is the bottom of the pile: the video tracks from the last one up.
    for (std::size_t r = film.tracks.size(); r-- > 0;) {
        const Track& tr = film.tracks[r];
        if (tr.kind != TrackKind::Video || tr.hidden) continue;
        for (const Film::Active& a : film.activeAt(r, t)) {
            const Clip& c = *a.clip;
            const Look lk = c.lookAt(t);
            const float alpha = std::clamp(lk.opacity, 0.0f, 1.0f) * fadeAt(c, t) * a.dissolve;
            if (alpha <= 0.0f) continue;
            const float cx = w * 0.5f + lk.x * w, cy = h * 0.5f + lk.y * h;
            switch (c.kind) {
            case ClipKind::Video: {
                std::shared_ptr<const MediaInfo> mi;
                if (waitMs < 0) mi = std::make_shared<MediaInfo>(lib.infoNow(c.file));
                else            mi = lib.info(c.file);
                if (!mi) {
                    if (report) report->complete = false;
                    break;
                }
                if (!mi->ok) { problem(c, mi->error); break; }
                if (!mi->video) break;   // a sound file on a video track: nothing to see
                if (mi->still && isImageFile(c.file)) break;
                bool exact = true;
                std::string why;
                const ImagePtr f = frames.video(c, *mi, t, film.fps, w, h, waitMs, &exact, &why);
                if (!exact && report) report->complete = false;
                if (!f) {
                    if (!why.empty()) problem(c, why);
                    else if (waitMs < 0) problem(c, "no frames");
                    break;
                }
                float dw = 0, dh = 0;
                fitSize(c.fit, mi->width, mi->height, w, h, dw, dh);
                drawLayer(out, *f, cx, cy, dw * lk.scale, dh * lk.scale, lk.rotation, alpha);
                break;
            }
            case ClipKind::Image: {
                const std::shared_ptr<const MediaInfo> mi =
                    waitMs < 0 ? std::make_shared<MediaInfo>(lib.infoNow(c.file)) : lib.info(c.file);
                if (!mi) {
                    if (report) report->complete = false;
                    break;
                }
                if (!mi->ok) { problem(c, mi->error); break; }
                float dw = 0, dh = 0;
                fitSize(c.fit, mi->width, mi->height, w, h, dw, dh);
                const ImagePtr img = lib.picture(c.file, static_cast<int>(dw * lk.scale), static_cast<int>(dh * lk.scale));
                if (!img) { problem(c, "cannot read the picture"); break; }
                drawLayer(out, *img, cx, cy, dw * lk.scale, dh * lk.scale, lk.rotation, alpha);
                break;
            }
            case ClipKind::Text: {
                const ImagePtr img = textImage(c.text, h);
                if (!img) {
                    if (!c.text.text.empty()) problem(c, "no font to set it in");
                    break;
                }
                drawLayer(out, *img, cx, cy, img->w * lk.scale, img->h * lk.scale, lk.rotation, alpha);
                break;
            }
            case ClipKind::Color: {
                Image px;
                px.w = px.h = 4;
                px.px.resize(64);
                for (int i = 0; i < 16; ++i) {
                    px.px[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(c.color.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                    px.px[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(c.color.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                    px.px[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(c.color.b, 0.0f, 1.0f) * 255.0f + 0.5f);
                    px.px[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(c.color.a, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                drawLayer(out, px, cx, cy, w * lk.scale, h * lk.scale, lk.rotation, alpha);
                break;
            }
            case ClipKind::Audio: break;
            }
        }
    }
}

// --- Sound ---------------------------------------------------------------------------------

std::vector<float> mixdown(const Film& film, double from, double to, Library& lib, bool wait, bool* complete) {
    if (complete) *complete = true;
    from = std::max(0.0, from);
    const std::int64_t n0 = std::llround(from * kRate);
    const std::int64_t n1 = std::max(n0, static_cast<std::int64_t>(std::llround(to * kRate)));
    std::vector<float> out(static_cast<std::size_t>(n1 - n0) * 2, 0.0f);
    for (const Track& tr : film.tracks) {
        if (tr.muted) continue;
        for (std::size_t ci = 0; ci < tr.clips.size(); ++ci) {
            const Clip& c = tr.clips[ci];
            if (!c.audible() || c.volume <= 0.0f) continue;
            const std::int64_t a = std::max(n0, static_cast<std::int64_t>(std::llround(c.start * kRate)));
            const std::int64_t b = std::min(n1, static_cast<std::int64_t>(std::llround(c.end() * kRate)));
            if (a >= b) continue;
            const std::shared_ptr<const Sound> s = wait ? lib.soundNow(c.file) : lib.sound(c.file);
            if (!s || !s->ready) {
                if (complete && !(s && s->failed)) *complete = false;
                continue;
            }
            if (s->pcm.empty()) continue;
            // The neighbours it overlaps on this track: a cross-fade, equal power.
            double xin0 = -1, xin1 = -1, xout0 = -1, xout1 = -1;
            if (ci > 0 && tr.clips[ci - 1].end() > c.start + 1e-6) {
                xin0 = c.start;
                xin1 = std::min(c.end(), tr.clips[ci - 1].end());
            }
            if (ci + 1 < tr.clips.size() && tr.clips[ci + 1].start < c.end() - 1e-6) {
                xout0 = std::max(c.start, tr.clips[ci + 1].start);
                xout1 = c.end();
            }
            const float gain = c.volume * tr.volume;
            const std::int64_t frames = s->frames();
            const float* pcm = s->pcm.data();
            for (std::int64_t n = a; n < b; ++n) {
                const double t = static_cast<double>(n) / kRate;
                float g = gain * fadeAt(c, t);
                if (xin1 > xin0 && t < xin1) g *= std::sin(0.5f * kPi * static_cast<float>(std::clamp((t - xin0) / (xin1 - xin0), 0.0, 1.0)));
                if (xout1 > xout0 && t >= xout0) g *= std::cos(0.5f * kPi * static_cast<float>(std::clamp((t - xout0) / (xout1 - xout0), 0.0, 1.0)));
                if (g <= 0.0f) continue;
                const double sp = c.sourceAt(t) * kRate;
                if (sp < 0.0) continue;
                const std::int64_t i0 = static_cast<std::int64_t>(sp);
                if (i0 >= frames) break;
                const std::int64_t i1 = std::min(i0 + 1, frames - 1);
                const float f = static_cast<float>(sp - i0);
                float* o = &out[static_cast<std::size_t>(n - n0) * 2];
                o[0] += g * (pcm[i0 * 2] + (pcm[i1 * 2] - pcm[i0 * 2]) * f);
                o[1] += g * (pcm[i0 * 2 + 1] + (pcm[i1 * 2 + 1] - pcm[i0 * 2 + 1]) * f);
            }
        }
    }
    return out;
}

// --- Preview ----------------------------------------------------------------------------------

Preview::Preview(Library& lib) : m_lib(lib), m_frames(lib) { m_thread = std::thread([this] { run(); }); }

Preview::~Preview() {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void Preview::request(std::shared_ptr<const Film> film, std::uint64_t rev, double t, int w, int h, bool playing) {
    std::lock_guard<std::mutex> lk(m_mx);
    m_ask     = {std::move(film), rev, t, w, h, playing};
    m_pending = true;
    m_cv.notify_all();
}

bool Preview::take(Image& out, double& t, std::uint64_t& rev, bool& complete) {
    std::lock_guard<std::mutex> lk(m_mx);
    if (!m_have) return false;
    out.w = m_done.w;
    out.h = m_done.h;
    out.px.swap(m_done.px);
    t        = m_doneT;
    rev      = m_doneRev;
    complete = m_doneComplete;
    m_have   = false;
    return true;
}

bool Preview::busy() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_pending || m_drawing;
}

std::vector<std::string> Preview::problems() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_problems;
}

void Preview::run() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    std::unique_lock<std::mutex> lk(m_mx);
    for (;;) {
        m_cv.wait(lk, [&] { return m_stop || m_pending; });
        if (m_stop) break;
        Ask a = m_ask;
        m_pending = false;
        m_drawing = true;
        lk.unlock();

        m_frames.clock = std::chrono::duration<double>(clock::now() - t0).count();
        Image img;
        DrawReport rep;
        // Playing, a frame may wait about as long as it is on screen; still,
        // a little longer (a seek is a new ffmpeg and a run up to the frame).
        drawFrame(*a.film, a.t, a.w, a.h, m_lib, m_frames, a.playing ? 120 : 450, img, &rep);
        // Clips that begin in the next second and a half: their ffmpeg starts
        // now, so the cut does not wait for it.
        std::vector<int> alive;
        for (const Track& tr : a.film->tracks)
            for (const Clip& c : tr.clips) {
                alive.push_back(c.uid);
                if (!a.playing || c.kind != ClipKind::Video || tr.hidden) continue;
                if (c.start > a.t && c.start < a.t + 1.5)
                    if (auto mi = m_lib.info(c.file); mi && mi->ok && mi->video && !mi->still)
                        m_frames.prefetch(c, *mi, c.start, a.film->fps, a.w, a.h);
            }
        m_frames.drop(m_frames.clock, 4.0, alive);

        lk.lock();
        m_done         = std::move(img);
        m_doneT        = a.t;
        m_doneRev      = a.rev;
        m_doneComplete = rep.complete;
        m_have         = true;
        m_problems     = rep.problems;
        m_drawing      = false;
        // Something was not there yet (a probe, a seek): draw it again once
        // more has come in -- unless something newer was asked for already.
        if (!rep.complete && !m_pending && !m_stop) {
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            lk.lock();
            if (!m_pending) {
                m_ask     = a;
                m_pending = true;
            }
        }
    }
}

// --- Export ---------------------------------------------------------------------------------

const char* formatName(Format f) {
    switch (f) {
    case Format::Mp4:    return "MP4 (H.264)";
    case Format::Hevc:   return "MP4 (H.265)";
    case Format::WebM:   return "WebM (VP9)";
    case Format::ProRes: return "MOV (ProRes, for editing)";
    case Format::Fvid:   return "Game video (.fvid)";
    case Format::Wav:    return "Sound only (WAV)";
    case Format::Count:  break;
    }
    return "?";
}

const char* formatExtension(Format f) {
    switch (f) {
    case Format::Mp4:
    case Format::Hevc:   return ".mp4";
    case Format::WebM:   return ".webm";
    case Format::ProRes: return ".mov";
    case Format::Fvid:   return ".fvid";
    case Format::Wav:    return ".wav";
    case Format::Count:  break;
    }
    return ".mp4";
}

bool formatNeedsFfmpeg(Format f) { return f != Format::Fvid && f != Format::Wav; }

Exporter::~Exporter() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

void Exporter::say(const std::string& stage) {
    std::lock_guard<std::mutex> lk(m_mx);
    m_stage = stage;
}

std::string Exporter::message() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_message;
}

std::string Exporter::stage() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_stage;
}

float Exporter::progress() const {
    const int total = m_total.load();
    return total > 0 ? std::clamp(static_cast<float>(m_done.load()) / total, 0.0f, 1.0f) : 0.0f;
}

void Exporter::cancel() {
    m_cancel = true;
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_enc) m_enc->kill();
}

bool Exporter::finished() {
    if (m_running.load() || !m_thread.joinable()) return false;
    m_thread.join();
    return true;
}

bool Exporter::start(const Film& film, const ExportSettings& s, Library& lib, std::string* error) {
    if (m_running.load()) {
        if (error) *error = "an export is already running";
        return false;
    }
    if (m_thread.joinable()) m_thread.join();
    if (s.path.empty()) {
        if (error) *error = "no file to write";
        return false;
    }
    if (formatNeedsFfmpeg(s.format) && !ffmpegReady()) {
        if (error) *error = std::string("this format needs ffmpeg -- ") + ffmpegInstallHint();
        return false;
    }
    m_set = s;
    m_set.width  = std::clamp(s.width, 16, 8192) & ~1;
    m_set.height = std::clamp(s.height, 16, 8192) & ~1;
    m_set.fps    = std::clamp(s.fps, 1.0, 240.0);
    m_cancel = false;
    m_ok     = false;
    m_done   = 0;
    m_total  = 0;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_message.clear();
        m_stage = "Starting";
    }
    m_running = true;
    m_thread  = std::thread([this, f = film, l = &lib]() mutable { run(std::move(f), l); });
    return true;
}

void Exporter::run(Film film, Library* libp) {
    Library& lib = *libp;
    const ExportSettings s = m_set;
    auto finish = [&](bool ok, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_message = msg;
            m_enc.reset();
        }
        m_ok      = ok;
        m_running = false;
    };
    std::error_code ec;
    const double dur = film.duration();
    const double from = std::clamp(s.from, 0.0, dur);
    const double to = s.to < 0.0 ? dur : std::clamp(s.to, from, dur);
    const int frames = static_cast<int>(std::llround((to - from) * s.fps));
    if (frames <= 0) return finish(false, "Nothing to export: the range is empty.");
    m_total = frames;
    if (fs::path(s.path).has_parent_path()) fs::create_directories(fs::path(s.path).parent_path(), ec);

    // Every source read once before the first frame: a probe or a sound that
    // fails says so now, not three minutes in.
    say("Reading the sources");
    for (const Track& tr : film.tracks)
        for (const Clip& c : tr.clips) {
            if (m_cancel) return finish(false, "Export cancelled.");
            if (c.end() <= from || c.start >= to || c.file.empty()) continue;
            if (c.visual() && !(tr.kind == TrackKind::Video && tr.hidden)) {
                const MediaInfo mi = lib.infoNow(c.file);
                if (!mi.ok) return finish(false, fs::path(c.file).filename().string() + ": " + mi.error);
            }
            if (s.sound && c.audible() && !tr.muted && s.format != Format::Fvid) {
                const auto snd = lib.soundNow(c.file);
                if (!snd) return finish(false, fs::path(c.file).filename().string() + ": the song is not bounced yet");
                if (snd->failed) return finish(false, fs::path(c.file).filename().string() + ": " + snd->error);
            }
        }

    // The sound first, all of it, into a scratch WAV ffmpeg reads beside the
    // pictures.
    const bool withSound = s.sound && s.format != Format::Fvid;
    fs::path wav;
    if (withSound) {
        say("Mixing the sound");
        fitzel::SampleData d;
        d.channels   = 2;
        d.sampleRate = kRate;
        d.samples    = mixdown(film, from, from + frames / s.fps, lib, true);
        for (float& v : d.samples) v = std::clamp(v, -1.0f, 1.0f);
        if (s.format == Format::Wav) {
            std::string err;
            if (!fitzel::saveWav(s.path, d, fitzel::WavFormat::Pcm24, &err)) return finish(false, err);
            m_done = frames;
            return finish(true, "Wrote " + fs::path(s.path).filename().string() + ".");
        }
        wav = fs::temp_directory_path(ec) / ("fitzel-film-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".wav");
        std::string err;
        if (!fitzel::saveWav(wav.string(), d, fitzel::WavFormat::Float32, &err)) return finish(false, err);
    } else if (s.format == Format::Wav) {
        return finish(false, "Sound only, with the sound off: nothing to write.");
    }
    struct RemoveWav {
        fs::path p;
        ~RemoveWav() { std::error_code e; if (!p.empty()) fs::remove(p, e); }
    } removeWav{wav};

    const int W = m_set.width, H = m_set.height;
    const std::size_t frameBytes = static_cast<std::size_t>(W) * H * 4;
    std::vector<unsigned char> fvid;                   // the JPEGs of a .fvid
    std::vector<std::pair<std::uint32_t, std::uint32_t>> fvidIndex;
    if (s.format != Format::Fvid) {
        const int q = std::clamp(s.quality, 1, 100);
        char rate[64];
        std::snprintf(rate, sizeof rate, "%lld/1000000", static_cast<long long>(std::llround(s.fps * 1e6)));
        std::vector<std::string> args{"-y", "-v", "error", "-nostdin", "-f", "rawvideo", "-pix_fmt", "rgba", "-s",
                                      std::to_string(W) + "x" + std::to_string(H), "-framerate", rate, "-i", "pipe:0"};
        if (withSound) args.insert(args.end(), {"-i", wav.string(), "-map", "0:v:0", "-map", "1:a:0"});
        switch (s.format) {
        case Format::Mp4:
            args.insert(args.end(), {"-c:v", "libx264", "-preset", "medium", "-crf", std::to_string(40 - q * 26 / 100),
                                     "-pix_fmt", "yuv420p", "-movflags", "+faststart"});
            if (withSound) args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k"});
            break;
        case Format::Hevc:
            args.insert(args.end(), {"-c:v", "libx265", "-preset", "medium", "-crf", std::to_string(42 - q * 26 / 100),
                                     "-pix_fmt", "yuv420p", "-tag:v", "hvc1", "-movflags", "+faststart", "-x265-params", "log-level=error"});
            if (withSound) args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k"});
            break;
        case Format::WebM:
            args.insert(args.end(), {"-c:v", "libvpx-vp9", "-crf", std::to_string(52 - q * 34 / 100), "-b:v", "0",
                                     "-row-mt", "1", "-deadline", "good", "-cpu-used", "2", "-pix_fmt", "yuv420p"});
            if (withSound) args.insert(args.end(), {"-c:a", "libopus", "-b:a", "160k"});
            break;
        case Format::ProRes:
            args.insert(args.end(), {"-c:v", "prores_ks", "-profile:v", q >= 90 ? "3" : q >= 60 ? "2" : "1",
                                     "-pix_fmt", "yuv422p10le"});
            if (withSound) args.insert(args.end(), {"-c:a", "pcm_s16le"});
            break;
        default: break;
        }
        args.push_back(s.path);
        auto enc = std::make_unique<Process>();
        std::string err;
        if (!enc->start(ffmpegExe(), args, true, false, &err)) return finish(false, err);
        std::lock_guard<std::mutex> lk(m_mx);
        m_enc = std::move(enc);
    }

    say("Drawing the frames");
    Frames src(lib);
    Image img;
    std::vector<int> alive;
    for (const Track& tr : film.tracks)
        for (const Clip& c : tr.clips) alive.push_back(c.uid);
    auto jpgOut = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* p = static_cast<const unsigned char*>(data);
        v->insert(v->end(), p, p + size);
    };
    std::vector<unsigned char> rgb;
    bool broke = false;
    for (int k = 0; k < frames; ++k) {
        if (m_cancel) break;
        const double t = from + k / s.fps;
        src.clock = t;
        drawFrame(film, t, W, H, lib, src, -1, img);
        // The clips that start within the next second begin decoding now.
        for (const Track& tr : film.tracks) {
            if (tr.kind != TrackKind::Video || tr.hidden) continue;
            for (const Clip& c : tr.clips)
                if (c.kind == ClipKind::Video && c.start > t && c.start <= t + 1.0)
                    if (auto mi = lib.info(c.file); mi && mi->ok && mi->video && !mi->still)
                        src.prefetch(c, *mi, c.start, film.fps, W, H);
        }
        src.drop(t, 2.0, alive);
        if (s.format == Format::Fvid) {
            rgb.resize(static_cast<std::size_t>(W) * H * 3);
            for (std::size_t i = 0, j = 0; i < frameBytes; i += 4, j += 3) {
                rgb[j] = img.px[i];
                rgb[j + 1] = img.px[i + 1];
                rgb[j + 2] = img.px[i + 2];
            }
            const std::size_t at = fvid.size();
            stbi_write_jpg_to_func(jpgOut, &fvid, W, H, 3, rgb.data(), std::clamp(s.quality, 10, 100));
            fvidIndex.emplace_back(static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(fvid.size() - at));
        } else {
            Process* enc = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                enc = m_enc.get();
            }
            if (!enc || !enc->write(img.px.data(), frameBytes)) {
                broke = true;
                break;
            }
        }
        m_done = k + 1;
    }

    if (m_cancel) {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (m_enc) m_enc->kill();
        }
        fs::remove(s.path, ec);
        return finish(false, "Export cancelled.");
    }

    if (s.format == Format::Fvid) {
        say("Writing the file");
        std::vector<unsigned char> head;
        auto u32 = [&head](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) head.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
        };
        head.insert(head.end(), fitzel::VideoTexture::kMagic, fitzel::VideoTexture::kMagic + 4);
        u32(fitzel::VideoTexture::kVersion);
        u32(static_cast<std::uint32_t>(W));
        u32(static_cast<std::uint32_t>(H));
        u32(static_cast<std::uint32_t>(fvidIndex.size()));
        const float fpsF = static_cast<float>(s.fps);
        std::uint32_t bits;
        std::memcpy(&bits, &fpsF, 4);
        u32(bits);
        const std::uint32_t base = static_cast<std::uint32_t>(head.size() + fvidIndex.size() * 8);
        for (const auto& [off, size] : fvidIndex) {
            u32(base + off);
            u32(size);
        }
        std::ofstream out(s.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        out.write(reinterpret_cast<const char*>(fvid.data()), static_cast<std::streamsize>(fvid.size()));
        if (!out) return finish(false, "Could not write " + s.path);
        return finish(true, "Wrote " + fs::path(s.path).filename().string() + " (" + std::to_string(frames) + " frames, " +
                                std::to_string(fvid.size() / 1024) + " KB).");
    }

    say("Finishing the file");
    int code = -1;
    std::string errs;
    {
        std::unique_ptr<Process> enc;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            enc = std::move(m_enc);
        }
        if (enc) {
            code = enc->wait();
            errs = trimmed(enc->errors());
        }
    }
    if (broke || code != 0) {
        fs::remove(s.path, ec);
        const auto nl = errs.find_last_of('\n');
        return finish(false, "ffmpeg stopped: " + (errs.empty() ? std::string("(no message)") : nl == std::string::npos ? errs : errs.substr(nl + 1)));
    }
    const auto bytes = fs::file_size(s.path, ec);
    char sz[64];
    std::snprintf(sz, sizeof sz, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    finish(true, "Wrote " + fs::path(s.path).filename().string() + " (" + std::to_string(frames) + " frames, " + sz + ").");
}

} // namespace vseq
