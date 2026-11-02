// The image editor's layer effects (ImageDoc.hpp: Fx, Effect, LayerStyle): their
// starting values, and the maps the compositor draws them from (ImageFx.hpp).
// The colour of an effect -- its blend mode, opacity, where it is clipped -- is
// the compositor's business, in ImageDoc.cpp; this file only works out where.
#include "ImageFx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ImagePixel.hpp"

namespace img {

const char* fxName(Fx f) {
    static const char* names[] = {"Drop shadow", "Outer glow", "Inner shadow", "Inner glow", "Bevel & emboss",
                                  "Color overlay", "Gradient overlay", "Stroke"};
    const int i = int(f);
    return i >= 0 && i < int(Fx::Count) ? names[i] : "?";
}

Effect defaultEffect(Fx f) {
    Effect e;
    e.on = true;
    switch (f) {
    case Fx::DropShadow:
    case Fx::InnerShadow:
        e.blend = Blend::Multiply; e.opacity = 0.75f; e.color = {0, 0, 0, 1};
        e.angle = 120.0f; e.distance = 5.0f; e.size = 5.0f;
        break;
    case Fx::OuterGlow:
    case Fx::InnerGlow:
        e.blend = Blend::Screen; e.opacity = 0.75f; e.color = {1, 1, 0.75f, 1}; e.size = 5.0f;
        break;
    case Fx::Bevel:
        e.blend = Blend::Screen; e.opacity = 0.75f; e.color = {1, 1, 1, 1};
        e.blend2 = Blend::Multiply; e.opacity2 = 0.75f; e.color2 = {0, 0, 0, 1};
        e.size = 5.0f; e.depth = 1.0f; e.angle = 120.0f; e.altitude = 30.0f;
        break;
    case Fx::ColorOverlay:
        e.color = {1, 0, 0, 1};
        break;
    case Fx::GradientOverlay:
        e.color = {0, 0, 0, 1}; e.color2 = {1, 1, 1, 1}; e.angle = 90.0f;
        break;
    case Fx::Stroke:
        e.color = {0, 0, 0, 1}; e.size = 3.0f;
        break;
    default:
        break;
    }
    return e;
}

LayerStyle::LayerStyle() {
    for (int f = 0; f < int(Fx::Count); ++f) {
        fx[f] = defaultEffect(Fx(f));
        fx[f].on = false;
    }
}

bool LayerStyle::any() const {
    if (hidden) return false;
    for (const Effect& e : fx) if (e.on) return true;
    return false;
}

namespace fx {

namespace {

const Effect* on(const LayerStyle& s, Fx f) { return s.fx[int(f)].on ? &s.fx[int(f)] : nullptr; }

int blurReach(float size) { return int(std::ceil(std::max(0.0f, size) * 1.5f)) + 2; }

// How far a change of the shape can move what the style draws, px.
int reachOf(const LayerStyle& s) {
    int r = 1;
    for (Fx f : {Fx::DropShadow, Fx::InnerShadow})
        if (const Effect* e = on(s, f)) r = std::max(r, int(std::ceil(e->distance)) + blurReach(e->size));
    for (Fx f : {Fx::OuterGlow, Fx::InnerGlow})
        if (const Effect* e = on(s, f)) r = std::max(r, blurReach(e->size));
    if (const Effect* e = on(s, Fx::Stroke)) r = std::max(r, int(std::ceil(e->size)) + 2);
    if (const Effect* e = on(s, Fx::Bevel))
        r = std::max(r, int(std::ceil(e->size)) + blurReach(e->size * 0.1f) + blurReach(e->soften) + 3);
    return r;
}

// The maps would come out the same: what is on, and every number that moves a
// shape (colours, blend modes and opacities are the compositor's).
bool sameGeometry(const LayerStyle& a, const LayerStyle& b) {
    if (a.any() != b.any()) return false;
    for (int f = 0; f < int(Fx::Count); ++f)
        if (a.fx[f].on != b.fx[f].on) return false;
    for (Fx f : {Fx::DropShadow, Fx::InnerShadow}) {
        const Effect &x = a.fx[int(f)], &y = b.fx[int(f)];
        if (x.on && (x.angle != y.angle || x.distance != y.distance || x.spread != y.spread || x.size != y.size)) return false;
    }
    for (Fx f : {Fx::OuterGlow, Fx::InnerGlow}) {
        const Effect &x = a.fx[int(f)], &y = b.fx[int(f)];
        if (x.on && (x.spread != y.spread || x.size != y.size || x.style != y.style)) return false;
    }
    const Effect &sx = a.fx[int(Fx::Stroke)], &sy = b.fx[int(Fx::Stroke)];
    if (sx.on && (sx.size != sy.size || sx.style != sy.style)) return false;
    const Effect &bx = a.fx[int(Fx::Bevel)], &by = b.fx[int(Fx::Bevel)];
    if (bx.on && (bx.style != by.style || bx.technique != by.technique || bx.depth != by.depth || bx.size != by.size ||
                  bx.soften != by.soften || bx.angle != by.angle || bx.altitude != by.altitude || bx.flip != by.flip))
        return false;
    return true;
}

// The layer's shape: its alpha through its mask.
void shapeOf(const Layer& l, int W, int H, std::vector<std::uint8_t>& out) {
    const std::size_t n = std::size_t(W) * H;
    out.resize(n);
    const std::uint8_t* p = l.px->data();
    if (l.mask && l.maskOn) {
        const std::uint8_t* m = l.mask->data();
        for (std::size_t i = 0; i < n; ++i) out[i] = std::uint8_t(p[i * 4 + 3] * px::maskValue(m + i * 4) / 255);
    } else {
        for (std::size_t i = 0; i < n; ++i) out[i] = p[i * 4 + 3];
    }
}

Rect boundsOf(const std::vector<std::uint8_t>& a, int W, int H) {
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* row = &a[std::size_t(y) * W];
        int l = 0;
        while (l < W && !row[l]) ++l;
        if (l == W) continue;
        int r = W - 1;
        while (!row[r]) --r;
        x0 = std::min(x0, l); x1 = std::max(x1, r);
        y0 = std::min(y0, y); y1 = y;
    }
    if (x1 < 0) return {};
    return {x0, y0, x1 + 1, y1 + 1};
}

// The box around every pixel where two shapes differ.
Rect changedRect(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int W, int H) {
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y) {
        const std::uint8_t *ra = &a[std::size_t(y) * W], *rb = &b[std::size_t(y) * W];
        if (std::memcmp(ra, rb, std::size_t(W)) == 0) continue;
        int l = 0;
        while (ra[l] == rb[l]) ++l;
        int r = W - 1;
        while (ra[r] == rb[r]) --r;
        x0 = std::min(x0, l); x1 = std::max(x1, r);
        y0 = std::min(y0, y); y1 = y;
    }
    if (x1 < 0) return {};
    return {x0, y0, x1 + 1, y1 + 1};
}

// The shape over `r` -- which may reach past the canvas, where it is empty -- as 0..1.
std::vector<float> shapeOver(const std::vector<std::uint8_t>& alpha, int W, int H, const Rect& r) {
    std::vector<float> f(std::size_t(r.w()) * r.h(), 0.0f);
    const Rect c = r.clipped(W, H);
    for (int y = c.y0; y < c.y1; ++y)
        for (int x = c.x0; x < c.x1; ++x)
            f[std::size_t(y - r.y0) * r.w() + (x - r.x0)] = alpha[std::size_t(y) * W + x] * px::k1_255;
    return f;
}

// f moved by (ox, oy).
std::vector<float> shifted(const std::vector<float>& f, int w, int h, int ox, int oy) {
    std::vector<float> o(f.size(), 0.0f);
    for (int y = 0; y < h; ++y) {
        const int sy = y - oy;
        if (sy < 0 || sy >= h) continue;
        for (int x = std::max(0, ox); x < std::min(w, w + ox); ++x) o[std::size_t(y) * w + x] = f[std::size_t(sy) * w + (x - ox)];
    }
    return o;
}

// The shape grown outwards by r px, anti-aliased; a distance field, so its corners round off.
void grow(std::vector<float>& f, int w, int h, float r) {
    std::vector<std::uint8_t> in(f.size());
    for (std::size_t i = 0; i < f.size(); ++i) in[i] = f[i] >= 0.5f;
    const std::vector<float> d2 = px::distance2(in, w, h);
    for (std::size_t i = 0; i < f.size(); ++i) f[i] = std::max(f[i], std::clamp(r + 1.0f - std::sqrt(d2[i]), 0.0f, 1.0f));
}

// Photoshop's spread and choke: that part of `size` grows the shape, the rest blurs it.
void spreadBlur(std::vector<float>& f, int w, int h, float size, float spread) {
    spread = std::clamp(spread, 0.0f, 1.0f);
    if (size * spread >= 0.5f) grow(f, w, h, size * spread);
    px::gaussBlur(f, w, h, 1, size * (1.0f - spread) * 0.5f);
}

// Signed distance to the shape's edge, px, negative inside. Whole pixels from the
// distance transform, the anti-aliased rim refining it below a pixel.
std::vector<float> signedEdge(const std::vector<float>& a, int w, int h) {
    std::vector<std::uint8_t> in(a.size()), out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) { in[i] = a[i] >= 0.5f; out[i] = !in[i]; }
    const std::vector<float> dOut = px::distance2(in, w, h), dIn = px::distance2(out, w, h);
    std::vector<float> sd(a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        sd[i] = in[i] ? 0.5f - std::sqrt(dIn[i]) + (1.0f - a[i]) : std::sqrt(dOut[i]) - 0.5f - a[i];
    return sd;
}

// The maps inside `out` (a part of the canvas), from the shape in e.alpha. The
// shape is read as far round `out` as the effects reach, so the result is the
// same as making the whole map.
void build(Entry& e, const LayerStyle& s, int W, int H, const Rect& out) {
    if (out.empty()) return;
    const int reach = reachOf(s);
    const Rect in{out.x0 - reach, out.y0 - reach, out.x1 + reach, out.y1 + reach};
    const int w = in.w(), h = in.h();
    const std::vector<float> A = shapeOver(e.alpha, W, H, in);
    auto store = [&](Map m, const std::vector<float>& f) {
        std::vector<std::uint8_t>& dst = e.map[m];
        if (dst.size() != std::size_t(W) * H) dst.assign(std::size_t(W) * H, 0);
        for (int y = out.y0; y < out.y1; ++y) {
            const float* src = &f[std::size_t(y - in.y0) * w + std::size_t(out.x0 - in.x0)];
            std::uint8_t* d = &dst[std::size_t(y) * W + out.x0];
            for (int x = 0; x < out.w(); ++x) d[x] = px::to8(src[x]);
        }
    };
    // A shadow falls away from the light.
    auto offset = [](const Effect& f, int& ox, int& oy) {
        const float a = f.angle * 0.017453293f;
        ox = int(std::lround(-std::cos(a) * f.distance));
        oy = int(std::lround(std::sin(a) * f.distance));
    };
    if (const Effect* f = on(s, Fx::DropShadow)) {
        int ox, oy;
        offset(*f, ox, oy);
        std::vector<float> m = shifted(A, w, h, ox, oy);
        spreadBlur(m, w, h, f->size, f->spread);
        store(DropShadow, m);
    }
    if (const Effect* f = on(s, Fx::InnerShadow)) {
        // The shadow the surroundings throw onto the shape: what is NOT shape, moved.
        int ox, oy;
        offset(*f, ox, oy);
        std::vector<float> m = shifted(A, w, h, ox, oy);
        for (float& v : m) v = 1.0f - v;
        spreadBlur(m, w, h, f->size, f->spread);
        store(InnerShadow, m);
    }
    if (const Effect* f = on(s, Fx::OuterGlow)) {
        std::vector<float> m = A;
        spreadBlur(m, w, h, f->size, f->spread);
        store(OuterGlow, m);
    }
    if (const Effect* f = on(s, Fx::InnerGlow)) {
        std::vector<float> m(A.size());
        for (std::size_t i = 0; i < A.size(); ++i) m[i] = 1.0f - A[i];
        spreadBlur(m, w, h, f->size, f->spread);
        if (f->style == 1) for (float& v : m) v = 1.0f - v;   // from the centre out
        store(InnerGlow, m);
    }
    std::vector<float> sd;
    if (on(s, Fx::Stroke) || on(s, Fx::Bevel)) sd = signedEdge(A, w, h);
    if (const Effect* f = on(s, Fx::Stroke)) {
        const float sz = std::max(0.5f, f->size);
        const float lo = f->style == 0 ? 0.0f : f->style == 1 ? -sz : -sz * 0.5f, hi = lo + sz;
        std::vector<float> m(sd.size());
        for (std::size_t i = 0; i < sd.size(); ++i) m[i] = std::clamp(std::min(sd[i] - lo, hi - sd[i]) + 0.5f, 0.0f, 1.0f);
        store(StrokeBand, m);
    }
    if (const Effect* f = on(s, Fx::Bevel)) {
        // A height field from the distance to the edge, lit from the angle and the altitude.
        const float sz = std::max(1.0f, f->size);
        std::vector<float> z(sd.size());
        for (std::size_t i = 0; i < sd.size(); ++i) {
            float t;
            switch (f->style) {
            case 1:  t = 1.0f - sd[i] / sz; break;               // outer: falls away outside the edge
            case 2:  t = 0.5f - sd[i] / sz; break;               // emboss: across the edge
            case 3:  t = std::fabs(sd[i]) / (sz * 0.5f); break;  // pillow: the edge pressed in
            default: t = -sd[i] / sz; break;                     // inner: rises inside the edge
            }
            t = std::clamp(t, 0.0f, 1.0f);
            const float hgt = f->technique == 1 ? t : std::sin(t * 1.5707963f);   // chisel: flat faces; smooth: rounded
            z[i] = hgt * sz * f->depth * (f->flip ? -1.0f : 1.0f);
        }
        if (f->technique == 0) px::gaussBlur(z, w, h, 1, std::max(0.5f, sz * 0.1f));
        const float az = f->angle * 0.017453293f, el = std::clamp(f->altitude, 0.0f, 90.0f) * 0.017453293f;
        const float lx = std::cos(el) * std::cos(az), ly = -std::cos(el) * std::sin(az), lz = std::sin(el);
        const float hiK = 1.0f / std::max(0.05f, 1.0f - lz), loK = 1.0f / std::max(0.1f, (std::cos(el) + lz) * 0.7f);
        std::vector<float> hiM(z.size()), loM(z.size());
        auto Z = [&](int x, int y) { return z[std::size_t(std::clamp(y, 0, h - 1)) * w + std::size_t(std::clamp(x, 0, w - 1))]; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float gx = (Z(x + 1, y) - Z(x - 1, y)) * 0.5f, gy = (Z(x, y + 1) - Z(x, y - 1)) * 0.5f;
                const float inv = 1.0f / std::sqrt(gx * gx + gy * gy + 1.0f);
                const float shade = (-gx * lx - gy * ly + lz) * inv - lz;   // 0 on the flat
                const std::size_t i = std::size_t(y) * w + x;
                hiM[i] = std::clamp(shade * hiK, 0.0f, 1.0f);
                loM[i] = std::clamp(-shade * loK, 0.0f, 1.0f);
            }
        if (f->soften > 0.0f) {
            px::gaussBlur(hiM, w, h, 1, f->soften * 0.5f);
            px::gaussBlur(loM, w, h, 1, f->soften * 0.5f);
        }
        store(BevelHi, hiM);
        store(BevelLo, loM);
    }
}

// The gradient overlay's line (or circle) across the layer's painted box.
void gradientOf(Entry& e, const Effect& f) {
    const Rect b = e.bounds.empty() ? Rect{0, 0, e.w, e.h} : e.bounds;
    e.cx = (b.x0 + b.x1) * 0.5f;
    e.cy = (b.y0 + b.y1) * 0.5f;
    const float sc = std::max(0.05f, f.scale);
    e.rInv = 1.0f / std::max(1.0f, std::max(b.w(), b.h()) * 0.5f * sc);
    const float a = f.angle * 0.017453293f, dx = std::cos(a), dy = -std::sin(a);
    const float half = std::max(0.5f, (std::fabs(dx) * b.w() + std::fabs(dy) * b.h()) * 0.5f * sc);
    e.gx = dx / (2.0f * half);
    e.gy = dy / (2.0f * half);
    e.g0 = 0.5f - (e.cx - 0.5f) * e.gx - (e.cy - 0.5f) * e.gy;
}

} // namespace

void ensure(Cache& c, const std::vector<Layer>& layers, int W, int H) {
    c.layers.resize(layers.size());
    std::vector<std::uint8_t> a;
    for (std::size_t k = 0; k < layers.size(); ++k) {
        Entry& e = c.layers[k];
        const Layer& l = layers[k];
        if (!l.style.any()) {
            if (e.w) e = Entry{};
            continue;
        }
        const bool sameSize = e.w == W && e.h == H && !e.alpha.empty();
        const bool sameSrc = sameSize && e.px.lock().get() == l.px.get() && e.mask.lock().get() == l.mask.get() &&
                             e.gen == l.gen && e.maskOn == l.maskOn;
        const bool sameGeo = sameSize && sameGeometry(e.style, l.style);
        if (!sameSrc || !sameGeo) {
            shapeOf(l, W, H, a);
            if (sameGeo) {
                // The same effects on a changed shape: redo them only round the change.
                const Rect r = changedRect(a, e.alpha, W, H);
                if (!r.empty()) {
                    e.alpha.swap(a);
                    const int reach = reachOf(l.style);
                    build(e, l.style, W, H, Rect{r.x0 - reach, r.y0 - reach, r.x1 + reach, r.y1 + reach}.clipped(W, H));
                    e.bounds = boundsOf(e.alpha, W, H);
                }
            } else {
                // Other effects (or other numbers): all of it, as far as they reach from
                // the shape. Beyond that the maps stay empty.
                e.alpha.swap(a);
                e.w = W;
                e.h = H;
                for (auto& m : e.map) m.clear();
                e.bounds = boundsOf(e.alpha, W, H);
                if (!e.bounds.empty()) {
                    const int reach = reachOf(l.style);
                    const Rect& b = e.bounds;
                    build(e, l.style, W, H, Rect{b.x0 - reach, b.y0 - reach, b.x1 + reach, b.y1 + reach}.clipped(W, H));
                }
            }
            e.px = l.px;
            e.mask = l.mask;
            e.gen = l.gen;
            e.maskOn = l.maskOn;
        }
        e.style = l.style;
        gradientOf(e, l.style.fx[int(Fx::GradientOverlay)]);
    }
}

} // namespace fx

} // namespace img
