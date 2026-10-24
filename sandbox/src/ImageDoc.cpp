#include "ImageDoc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <stb_image.h>
#include <stb_image_write.h>

#include "ImagePixel.hpp"

namespace fs = std::filesystem;

namespace img {

// --- Rect -----------------------------------------------------------------------------

void Rect::add(const Rect& o) {
    if (o.empty()) return;
    if (empty()) { *this = o; return; }
    x0 = std::min(x0, o.x0); y0 = std::min(y0, o.y0);
    x1 = std::max(x1, o.x1); y1 = std::max(y1, o.y1);
}

Rect Rect::clipped(int w, int h) const {
    Rect r{std::max(x0, 0), std::max(y0, 0), std::min(x1, w), std::min(y1, h)};
    if (r.empty()) return {};
    return r;
}

Rect Rect::around(float x, float y, float r) {
    return {int(std::floor(x - r - 1.0f)), int(std::floor(y - r - 1.0f)),
            int(std::ceil(x + r + 1.0f)),  int(std::ceil(y + r + 1.0f))};
}

void Xform::apply(float x, float y, float& ox, float& oy) const {
    const float a = rot * 0.017453293f, c = std::cos(a), s = std::sin(a);
    const float qx = (x - px) * sx, qy = (y - py) * sy;
    ox = c * qx - s * qy + px + tx;
    oy = s * qx + c * qy + py + ty;
}

const char* blendName(Blend b) {
    static const char* names[] = {"Normal", "Multiply", "Screen", "Overlay", "Soft light", "Add",
                                  "Subtract", "Darken", "Lighten", "Difference", "Color"};
    const int i = int(b);
    return i >= 0 && i < int(Blend::Count) ? names[i] : "?";
}

bool isImageFile(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" ||
           e == ".psd" || e == ".gif";
}

// --- Blending -------------------------------------------------------------------------

namespace {

float softLight(float cb, float cs) {
    if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
    const float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb);
    return cb + (2.0f * cs - 1.0f) * (d - cb);
}

float blendCh(Blend b, float cb, float cs) {
    switch (b) {
    case Blend::Multiply:   return cb * cs;
    case Blend::Screen:     return cb + cs - cb * cs;
    case Blend::Overlay:    return cb <= 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case Blend::SoftLight:  return softLight(cb, cs);
    case Blend::Add:        return std::min(1.0f, cb + cs);
    case Blend::Subtract:   return std::max(0.0f, cb - cs);
    case Blend::Darken:     return std::min(cb, cs);
    case Blend::Lighten:    return std::max(cb, cs);
    case Blend::Difference: return std::fabs(cb - cs);
    default:                return cs;
    }
}

float lum(float r, float g, float b) { return 0.3f * r + 0.59f * g + 0.11f * b; }

// W3C "Color": the source's hue and saturation at the backdrop's luminosity.
void setLum(float& r, float& g, float& b, float l) {
    const float d = l - lum(r, g, b);
    r += d; g += d; b += d;
    const float L = lum(r, g, b);
    const float n = std::min({r, g, b}), x = std::max({r, g, b});
    if (n < 0.0f) {
        const float k = L / std::max(1e-6f, L - n);
        r = L + (r - L) * k; g = L + (g - L) * k; b = L + (b - L) * k;
    }
    if (x > 1.0f) {
        const float k = (1.0f - L) / std::max(1e-6f, x - L);
        r = L + (r - L) * k; g = L + (g - L) * k; b = L + (b - L) * k;
    }
}

void compositeAt(const std::vector<Layer>& layers, std::size_t i, std::uint8_t* out);

} // namespace

// --- Life -----------------------------------------------------------------------------

void Document::reshaped(int w, int h) {
    m_w = w; m_h = h;
    ++m_shapeVersion;
    dirtyAll();
}

void Document::create(int w, int h, Color fill) {
    w = std::max(1, w); h = std::max(1, h);
    Layer l;
    l.name = "Background";
    l.px = std::make_shared<Pixels>(std::size_t(w) * h * 4);
    const std::uint8_t c[4] = {px::to8(fill.r), px::to8(fill.g), px::to8(fill.b), px::to8(fill.a)};
    for (std::size_t i = 0; i < l.px->size(); i += 4) std::memcpy(&(*l.px)[i], c, 4);
    m_layers.clear();
    m_layers.push_back(std::move(l));
    m_active = 0;
    m_sel.reset(); m_selBounds = {}; ++m_selVersion;
    m_undo.clear(); m_redo.clear(); m_lastKey.clear();
    m_editing = false; m_base.reset(); m_baseSel.reset();
    m_path.clear();
    m_version = m_savedVersion = ++m_counter;
    reshaped(w, h);
}

void Document::adopt(int w, int h, Pixels pixels, const std::string& name) {
    create(w, h, Color{0, 0, 0, 0});
    *m_layers[0].px = std::move(pixels);
    m_layers[0].name = name.empty() ? std::string("Background") : name;
    dirtyAll();
}

static bool readFile(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool Document::load(const std::string& path, std::string& err) {
    std::vector<unsigned char> bytes;
    if (!readFile(path, bytes)) { err = "Could not read " + path; return false; }
    std::string e = fs::path(path).extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".psd") {
        // Its layers if we can read them; otherwise stb's merged picture below.
        PsdLayers pl;
        std::string perr;
        if (readPsd(bytes, pl, perr) && !pl.layers.empty()) {
            create(pl.w, pl.h, Color{0, 0, 0, 0});
            m_layers = std::move(pl.layers);
            m_active = int(m_layers.size()) - 1;
            m_path = fs::path(path).generic_string();
            dirtyAll();
            return true;
        }
    }
    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    unsigned char* data = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!data) { err = std::string("Not an image this editor reads (") + stbi_failure_reason() + ")"; return false; }
    Pixels p(data, data + std::size_t(w) * h * 4);
    stbi_image_free(data);
    adopt(w, h, std::move(p), "Background");
    m_path = fs::path(path).generic_string();
    return true;
}

static void appendBytes(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<unsigned char>*>(ctx);
    const auto* b = static_cast<const unsigned char*>(data);
    v->insert(v->end(), b, b + size);
}

bool Document::save(const std::string& path, std::string& err, int jpgQuality) {
    std::string e = fs::path(path).extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    Pixels flat = flattened();
    std::vector<unsigned char> bytes;
    int ok = 0;
    if (e == ".png") {
        ok = stbi_write_png_to_func(appendBytes, &bytes, m_w, m_h, 4, flat.data(), m_w * 4);
    } else if (e == ".jpg" || e == ".jpeg") {
        // JPEG has no alpha: what is see-through goes onto white, as it would print.
        Pixels rgb(std::size_t(m_w) * m_h * 3);
        for (std::size_t i = 0, j = 0; i < flat.size(); i += 4, j += 3) {
            const float a = flat[i + 3] / 255.0f;
            for (int c = 0; c < 3; ++c) rgb[j + c] = px::to8(flat[i + c] / 255.0f * a + (1.0f - a));
        }
        ok = stbi_write_jpg_to_func(appendBytes, &bytes, m_w, m_h, 3, rgb.data(), std::clamp(jpgQuality, 1, 100));
    } else if (e == ".tga") {
        ok = stbi_write_tga_to_func(appendBytes, &bytes, m_w, m_h, 4, flat.data());
    } else if (e == ".bmp") {
        ok = stbi_write_bmp_to_func(appendBytes, &bytes, m_w, m_h, 4, flat.data());
    } else if (e == ".psd") {
        PsdLayers pl;
        pl.w = m_w; pl.h = m_h;
        pl.layers = m_layers;
        ok = writePsd(pl, flat, bytes);
    } else {
        err = "Save as .png, .jpg, .tga, .bmp or .psd";
        return false;
    }
    if (!ok || bytes.empty()) { err = "Could not encode the picture"; return false; }
    // Written beside it and swapped in, so a texture watcher never reads half a file.
    const fs::path target(path), tmp = fs::path(path + ".saving");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "Could not write " + path; return false; }
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) { err = "Could not write " + path; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "Could not replace " + path;
        return false;
    }
    m_path = target.generic_string();
    m_savedVersion = m_version;
    return true;
}

std::string Document::title() const {
    return m_path.empty() ? std::string("Untitled") : fs::path(m_path).filename().string();
}

// --- Layers ---------------------------------------------------------------------------

void Document::setActive(int i) {
    if (m_editing) endEdit();
    m_active = std::clamp(i, 0, layerCount() - 1);
}

std::uint8_t* Document::pixelsMut(int i) {
    auto& p = m_layers[std::size_t(i)].px;
    if (p.use_count() > 1) p = std::make_shared<Pixels>(*p);
    return p->data();
}

void Document::setOpacity(int i, float o) {
    checkpoint("Layer opacity", "opacity" + std::to_string(i));
    m_layers[std::size_t(i)].opacity = std::clamp(o, 0.0f, 1.0f);
    dirtyAll();
}

void Document::setBlend(int i, Blend b) {
    checkpoint("Blend mode");
    m_layers[std::size_t(i)].blend = b;
    dirtyAll();
}

void Document::setVisible(int i, bool v) {
    checkpoint(v ? "Show layer" : "Hide layer");
    m_layers[std::size_t(i)].visible = v;
    dirtyAll();
}

void Document::rename(int i, const std::string& name, bool undoable) {
    if (m_layers[std::size_t(i)].name == name) return;
    if (undoable) checkpoint("Rename layer", "rename" + std::to_string(i));
    m_layers[std::size_t(i)].name = name;
}

void Document::setLockAlpha(int i, bool on) {
    checkpoint(on ? "Lock transparency" : "Unlock transparency");
    m_layers[std::size_t(i)].lockAlpha = on;
}

void Document::addLayer(const std::string& name) {
    checkpoint("New layer");
    Layer l;
    l.name = name.empty() ? "Layer " + std::to_string(layerCount()) : name;
    l.px = std::make_shared<Pixels>(std::size_t(m_w) * m_h * 4, std::uint8_t(0));
    m_layers.insert(m_layers.begin() + m_active + 1, std::move(l));
    ++m_active;
}

void Document::duplicateLayer() {
    checkpoint("Duplicate layer");
    Layer l = m_layers[std::size_t(m_active)];   // shares the pixels until one is painted
    l.name += " copy";
    m_layers.insert(m_layers.begin() + m_active + 1, std::move(l));
    ++m_active;
    dirtyAll();
}

void Document::deleteLayer() {
    if (layerCount() <= 1) return;
    checkpoint("Delete layer");
    m_layers.erase(m_layers.begin() + m_active);
    m_active = std::min(m_active, layerCount() - 1);
    dirtyAll();
}

void Document::moveLayer(int dir) {
    const int to = m_active + (dir > 0 ? 1 : -1);
    if (to < 0 || to >= layerCount()) return;
    checkpoint(dir > 0 ? "Layer up" : "Layer down");
    std::swap(m_layers[std::size_t(m_active)], m_layers[std::size_t(to)]);
    m_active = to;
    dirtyAll();
}

void Document::moveLayerTo(int from, int to) {
    from = std::clamp(from, 0, layerCount() - 1);
    to = std::clamp(to, 0, layerCount() - 1);
    if (from == to) return;
    checkpoint("Move layer");
    Layer l = std::move(m_layers[std::size_t(from)]);
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, std::move(l));
    m_active = to;
    dirtyAll();
}

void Document::layerViaCopy(bool cut) {
    checkpoint(cut ? "Layer via cut" : "Layer via copy");
    const std::size_t n = std::size_t(m_w) * m_h;
    Layer l;
    l.name = "Layer " + std::to_string(layerCount());
    l.px = std::make_shared<Pixels>(pixels(m_active));
    if (hasSelection()) {
        Pixels& p = *l.px;
        for (std::size_t i = 0; i < n; ++i) p[i * 4 + 3] = std::uint8_t(p[i * 4 + 3] * selAt(i) / 255);
        if (cut) {
            std::uint8_t* d = pixelsMut(m_active);
            for (std::size_t i = 0; i < n; ++i) d[i * 4 + 3] = std::uint8_t(d[i * 4 + 3] * (255 - selAt(i)) / 255);
        }
    } else if (cut) {
        std::memset(pixelsMut(m_active), 0, n * 4);
    }
    m_layers.insert(m_layers.begin() + m_active + 1, std::move(l));
    ++m_active;
    dirtyAll();
}

void Document::mergeVisible() {
    int shown = 0, lowest = -1;
    for (int i = 0; i < layerCount(); ++i)
        if (m_layers[std::size_t(i)].visible) { ++shown; if (lowest < 0) lowest = i; }
    if (shown < 2) return;
    checkpoint("Merge visible");
    Layer l;
    l.name = m_layers[std::size_t(lowest)].name;
    l.px = std::make_shared<Pixels>(flattened());   // the hidden ones add nothing to it
    std::vector<Layer> out;
    for (int i = 0; i < layerCount(); ++i) {
        if (i == lowest) out.push_back(l);
        else if (!m_layers[std::size_t(i)].visible) out.push_back(m_layers[std::size_t(i)]);
    }
    m_layers = std::move(out);
    m_active = 0;
    for (int i = 0; i < layerCount(); ++i) if (m_layers[std::size_t(i)].px == l.px) m_active = i;
    dirtyAll();
}

void Document::mergeDown() {
    if (m_active <= 0) return;
    checkpoint("Merge down");
    // The two layers composited on their own become the lower one. Its opacity
    // is baked in (it becomes 100 %); its blend mode against what lies below stays.
    Layer& lower = m_layers[std::size_t(m_active - 1)];
    Layer under = lower;
    under.blend = Blend::Normal;
    const std::vector<Layer> two{under, m_layers[std::size_t(m_active)]};
    auto merged = std::make_shared<Pixels>(std::size_t(m_w) * m_h * 4);
    for (std::size_t i = 0; i < merged->size(); i += 4) compositeAt(two, i, merged->data() + i);
    lower.px = std::move(merged);
    lower.opacity = 1.0f;
    lower.visible = true;
    m_layers.erase(m_layers.begin() + m_active);
    --m_active;
    dirtyAll();
}

void Document::flatten() {
    if (layerCount() == 1 && m_layers[0].opacity >= 1.0f && m_layers[0].visible &&
        m_layers[0].blend == Blend::Normal) return;
    checkpoint("Flatten");
    Layer l;
    l.name = "Background";
    l.px = std::make_shared<Pixels>(flattened());
    m_layers.clear();
    m_layers.push_back(std::move(l));
    m_active = 0;
    dirtyAll();
}

// --- Compositing ----------------------------------------------------------------------

namespace {

// One pixel (byte offset i) of a stack of layers, bottom first, composited
// over transparency the way the W3C compositing spec does it.
void compositeAt(const std::vector<Layer>& layers, std::size_t i, std::uint8_t* out) {
            float br = 0.0f, bg = 0.0f, bb = 0.0f, ba = 0.0f;
            for (const Layer& l : layers) {
                if (!l.visible || l.opacity <= 0.0f) continue;
                const std::uint8_t* s = l.px->data() + i;
                const float as = s[3] * px::k1_255 * l.opacity;
                if (as <= 0.0f) continue;
                float sr = s[0] * px::k1_255, sg = s[1] * px::k1_255, sb = s[2] * px::k1_255;
                float mr, mg, mb;
                if (l.blend == Blend::Normal || ba <= 0.0f) {
                    mr = sr; mg = sg; mb = sb;
                } else {
                    float xr, xg, xb;
                    if (l.blend == Blend::Color) {
                        xr = sr; xg = sg; xb = sb;
                        setLum(xr, xg, xb, lum(br, bg, bb));
                    } else {
                        xr = blendCh(l.blend, br, sr);
                        xg = blendCh(l.blend, bg, sg);
                        xb = blendCh(l.blend, bb, sb);
                    }
                    mr = (1.0f - ba) * sr + ba * xr;
                    mg = (1.0f - ba) * sg + ba * xg;
                    mb = (1.0f - ba) * sb + ba * xb;
                }
                const float ao = as + ba * (1.0f - as);
                const float kb = ba * (1.0f - as);
                br = (as * mr + kb * br) / ao;
                bg = (as * mg + kb * bg) / ao;
                bb = (as * mb + kb * bb) / ao;
                ba = ao;
            }
            out[0] = px::to8(br); out[1] = px::to8(bg);
            out[2] = px::to8(bb); out[3] = px::to8(ba);
}

} // namespace

void Document::composite(const Rect& rIn, std::uint8_t* out) const {
    const Rect r = rIn.clipped(m_w, m_h);
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) {
            const std::size_t i = (std::size_t(y) * m_w + x) * 4;
            compositeAt(m_layers, i, out + i);
        }
}

Pixels Document::flattened() const {
    Pixels out(std::size_t(m_w) * m_h * 4);
    composite({0, 0, m_w, m_h}, out.data());
    return out;
}

Color Document::sample(int x, int y, bool merged) const {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return {0, 0, 0, 0};
    const std::size_t i = (std::size_t(y) * m_w + x) * 4;
    std::uint8_t one[4];
    const std::uint8_t* p;
    if (merged) {
        compositeAt(m_layers, i, one);
        p = one;
    } else {
        p = pixels(m_active).data() + i;
    }
    return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
}

// --- Selection ------------------------------------------------------------------------

void Document::setSelection(std::shared_ptr<Mask> m) {
    ++m_selVersion;
    m_selBounds = {};
    if (!m || m->empty()) { m_sel.reset(); return; }
    int x0 = m_w, y0 = m_h, x1 = -1, y1 = -1;
    for (int y = 0; y < m_h; ++y) {
        const std::uint8_t* row = m->data() + std::size_t(y) * m_w;
        for (int x = 0; x < m_w; ++x)
            if (row[x]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    }
    if (x1 < 0) { m_sel.reset(); return; }
    m_sel = std::move(m);
    m_selBounds = {x0, y0, x1 + 1, y1 + 1};
}

void Document::combineSelection(Mask&& m, SelOp op) {
    const std::size_t n = std::size_t(m_w) * m_h;
    auto out = std::make_shared<Mask>(std::move(m));
    if (op != SelOp::Replace) {
        if (!hasSelection()) {
            // Adding to nothing is the new shape; taking away from (or meeting)
            // nothing leaves nothing.
            if (op != SelOp::Add) out->assign(n, 0);
        } else {
            const Mask& cur = *m_sel;
            for (std::size_t i = 0; i < n; ++i) {
                const int a = cur[i], b = (*out)[i];
                int v = a;
                if (op == SelOp::Add) v = std::max(a, b);
                else if (op == SelOp::Subtract) v = a * (255 - b) / 255;
                else v = std::min(a, b);
                (*out)[i] = std::uint8_t(v);
            }
        }
    }
    setSelection(std::move(out));
}

void Document::selectRect(const Rect& rIn, SelOp op, bool ellipse) {
    checkpoint(ellipse ? "Ellipse select" : "Rectangle select");
    const Rect r = rIn.clipped(m_w, m_h);
    Mask m(std::size_t(m_w) * m_h, 0);
    if (!ellipse) {
        for (int y = r.y0; y < r.y1; ++y)
            std::memset(m.data() + std::size_t(y) * m_w + r.x0, 255, std::size_t(r.w()));
    } else if (!rIn.empty()) {
        const float cx = (rIn.x0 + rIn.x1) * 0.5f, cy = (rIn.y0 + rIn.y1) * 0.5f;
        const float rx = rIn.w() * 0.5f, ry = rIn.h() * 0.5f;
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float cov = std::clamp((1.0f - d) * std::min(rx, ry) + 0.5f, 0.0f, 1.0f);
                m[std::size_t(y) * m_w + x] = px::to8(cov);
            }
    }
    combineSelection(std::move(m), op);
}

namespace {

// The pixels within `tol` of the seed's colour: 255 in, 0 out.
Mask colourRegion(const Pixels& src, int w, int h, int sx, int sy, float tol, bool contiguous) {
    Mask m(std::size_t(w) * h, 0);
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return m;
    const std::uint8_t* seed = src.data() + (std::size_t(sy) * w + sx) * 4;
    const int lim = int(std::lround(std::clamp(tol, 0.0f, 1.0f) * 255.0f));
    auto near = [&](std::size_t p) {
        const std::uint8_t* q = src.data() + p * 4;
        // Fully transparent pixels match each other whatever colour they hide.
        if (seed[3] == 0 && q[3] == 0) return true;
        for (int c = 0; c < 4; ++c)
            if (std::abs(int(q[c]) - int(seed[c])) > lim) return false;
        return true;
    };
    if (!contiguous) {
        for (std::size_t p = 0; p < m.size(); ++p) if (near(p)) m[p] = 255;
        return m;
    }
    // Scanline flood fill, 4-connected.
    std::vector<std::pair<int, int>> stack{{sx, sy}};
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        std::size_t row = std::size_t(y) * w;
        if (m[row + x] || !near(row + x)) continue;
        int l = x, r = x;
        while (l > 0 && !m[row + l - 1] && near(row + l - 1)) --l;
        while (r < w - 1 && !m[row + r + 1] && near(row + r + 1)) ++r;
        std::memset(m.data() + row + l, 255, std::size_t(r - l + 1));
        for (int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= h) continue;
            const std::size_t nrow = std::size_t(ny) * w;
            bool inRun = false;
            for (int i = l; i <= r; ++i) {
                const bool ok = !m[nrow + i] && near(nrow + i);
                if (ok && !inRun) stack.push_back({i, ny});
                inRun = ok;
            }
        }
    }
    return m;
}

} // namespace

void Document::selectColor(int x, int y, float tol, bool contiguous, bool merged, SelOp op) {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return;
    checkpoint("Magic wand");
    const Pixels src = merged ? flattened() : pixels(m_active);
    combineSelection(colourRegion(src, m_w, m_h, x, y, tol, contiguous), op);
}

void Document::selectPolygon(const std::vector<float>& xy, SelOp op) {
    const std::size_t np = xy.size() / 2;
    if (np < 3) return;
    checkpoint("Lasso");
    // Four sub-rows per pixel row, even-odd, coverage split exactly across the
    // pixels a span starts and ends in.
    Mask m(std::size_t(m_w) * m_h, 0);
    std::vector<float> acc(std::size_t(m_w) + 1), xs;
    float ylo = 1e30f, yhi = -1e30f;
    for (std::size_t i = 0; i < np; ++i) { ylo = std::min(ylo, xy[i * 2 + 1]); yhi = std::max(yhi, xy[i * 2 + 1]); }
    const int y0 = std::max(0, int(std::floor(ylo))), y1 = std::min(m_h, int(std::ceil(yhi)) + 1);
    const int sub = 4;
    for (int y = y0; y < y1; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        bool any = false;
        for (int k = 0; k < sub; ++k) {
            const float sy = y + (k + 0.5f) / sub;
            xs.clear();
            for (std::size_t i = 0; i < np; ++i) {
                const float ax = xy[i * 2], ay = xy[i * 2 + 1];
                const float bx = xy[((i + 1) % np) * 2], by = xy[((i + 1) % np) * 2 + 1];
                if ((ay <= sy) == (by <= sy)) continue;
                xs.push_back(ax + (sy - ay) / (by - ay) * (bx - ax));
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t j = 0; j + 1 < xs.size(); j += 2) {
                const float a = std::clamp(xs[j], 0.0f, float(m_w)), b = std::clamp(xs[j + 1], 0.0f, float(m_w));
                if (b <= a) continue;
                any = true;
                const int ia = int(a), ib = int(b);
                const float wgt = 1.0f / sub;
                if (ia == ib) { acc[std::size_t(ia)] += (b - a) * wgt; continue; }
                acc[std::size_t(ia)] += (ia + 1 - a) * wgt;
                for (int x = ia + 1; x < ib; ++x) acc[std::size_t(x)] += wgt;
                if (ib < m_w) acc[std::size_t(ib)] += (b - ib) * wgt;
            }
        }
        if (!any) continue;
        for (int x = 0; x < m_w; ++x) m[std::size_t(y) * m_w + x] = px::to8(acc[std::size_t(x)]);
    }
    combineSelection(std::move(m), op);
}

void Document::shiftSelection(const std::shared_ptr<Mask>& base, int dx, int dy) {
    if (!base) return;
    auto m = std::make_shared<Mask>(base->size(), 0);
    for (int y = 0; y < m_h; ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= m_h) continue;
        const int x0 = std::max(0, dx), x1 = std::min(m_w, m_w + dx);
        if (x1 <= x0) continue;
        std::memcpy(m->data() + std::size_t(y) * m_w + x0, base->data() + std::size_t(sy) * m_w + x0 - dx, std::size_t(x1 - x0));
    }
    setSelection(std::move(m));
}

void Document::reselect() {
    if (!m_lastSel) return;
    checkpoint("Reselect");
    setSelection(m_lastSel);
}

namespace {

// Squared distance, in pixels, from every pixel to the nearest pixel where
// `feature` is true (Felzenszwalb & Huttenlocher, one axis at a time).
std::vector<float> distance2(const std::vector<std::uint8_t>& feature, int w, int h) {
    const float inf = 1e20f;
    std::vector<float> d(std::size_t(w) * h);
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = feature[i] ? 0.0f : inf;
    const int n = std::max(w, h);
    std::vector<float> f(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1), out(static_cast<std::size_t>(n));
    std::vector<int> v(static_cast<std::size_t>(n));
    auto pass = [&](int len) {
        int k = 0;
        v[0] = 0; z[0] = -inf; z[1] = inf;
        for (int q = 1; q < len; ++q) {
            float s;
            for (;;) {
                const int r = v[std::size_t(k)];
                s = ((f[std::size_t(q)] + float(q) * q) - (f[std::size_t(r)] + float(r) * r)) / (2.0f * (q - r));
                if (s <= z[std::size_t(k)] && k > 0) { --k; continue; }
                break;
            }
            if (s <= z[std::size_t(k)]) { v[0] = q; z[0] = -inf; z[1] = inf; k = 0; continue; }
            ++k;
            v[std::size_t(k)] = q; z[std::size_t(k)] = s; z[std::size_t(k) + 1] = inf;
        }
        k = 0;
        for (int q = 0; q < len; ++q) {
            while (z[std::size_t(k) + 1] < q) ++k;
            const int r = v[std::size_t(k)];
            out[std::size_t(q)] = float(q - r) * (q - r) + f[std::size_t(r)];
        }
    };
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[std::size_t(y)] = d[std::size_t(y) * w + x];
        pass(h);
        for (int y = 0; y < h; ++y) d[std::size_t(y) * w + x] = out[std::size_t(y)];
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) f[std::size_t(x)] = d[std::size_t(y) * w + x];
        pass(w);
        for (int x = 0; x < w; ++x) d[std::size_t(y) * w + x] = out[std::size_t(x)];
    }
    return d;
}

// The signed distance to the selection's edge: negative inside, positive out.
std::vector<float> signedEdge(const Mask& m, int w, int h) {
    std::vector<std::uint8_t> in(m.size()), out(m.size());
    for (std::size_t i = 0; i < m.size(); ++i) { in[i] = m[i] >= 128; out[i] = !in[i]; }
    const std::vector<float> dOut = distance2(in, w, h);    // outside: to the nearest selected
    const std::vector<float> dIn = distance2(out, w, h);    // inside: to the nearest unselected
    std::vector<float> sd(m.size());
    for (std::size_t i = 0; i < m.size(); ++i)
        sd[i] = in[i] ? -(std::sqrt(dIn[i]) - 0.5f) : std::sqrt(dOut[i]) - 0.5f;
    return sd;
}

} // namespace

void Document::modifySelection(SelModify mod, float r) {
    if (!hasSelection() || r <= 0.0f) return;
    static const char* labels[] = {"Expand selection", "Contract selection", "Feather", "Border", "Smooth selection"};
    checkpoint(labels[int(mod)]);
    const std::size_t n = std::size_t(m_w) * m_h;
    auto m = std::make_shared<Mask>(n);
    if (mod == SelModify::Feather || mod == SelModify::Smooth) {
        std::vector<float> f(n);
        for (std::size_t i = 0; i < n; ++i) f[i] = (*m_sel)[i] * px::k1_255;
        px::gaussBlur(f, m_w, m_h, 1, mod == SelModify::Feather ? r * 0.5f : r * 0.35f);
        for (std::size_t i = 0; i < n; ++i)
            (*m)[i] = mod == SelModify::Feather ? px::to8(f[i]) : (f[i] >= 0.5f ? 255 : 0);
    } else {
        const std::vector<float> sd = signedEdge(*m_sel, m_w, m_h);
        for (std::size_t i = 0; i < n; ++i) {
            float c;
            if (mod == SelModify::Expand) c = r + 0.5f - sd[i] - 0.5f;
            else if (mod == SelModify::Contract) c = -sd[i] - r;
            else c = r * 0.5f + 0.5f - std::fabs(sd[i]);
            (*m)[i] = px::to8(std::clamp(c + 0.5f, 0.0f, 1.0f));
        }
    }
    setSelection(std::move(m));
}

void Document::strokeSelection(Color c, float width, int where) {
    if (!hasSelection() || width <= 0.0f) return;
    checkpoint("Stroke");
    const std::vector<float> sd = signedEdge(*m_sel, m_w, m_h);
    const float a = where == 0 ? -width : where == 1 ? -width * 0.5f : 0.0f, b = a + width;
    const bool lock = m_layers[std::size_t(m_active)].lockAlpha;
    std::uint8_t* d = pixelsMut(m_active);
    for (std::size_t i = 0; i < sd.size(); ++i) {
        const float cov = std::clamp(std::min(sd[i] - a, b - sd[i]) + 0.5f, 0.0f, 1.0f);
        if (cov > 0.0f) px::paint(d + i * 4, c.r, c.g, c.b, c.a * cov, lock);
    }
    dirtyAll();
}

void Document::selectAll() {
    checkpoint("Select all");
    setSelection(std::make_shared<Mask>(std::size_t(m_w) * m_h, 255));
}

void Document::deselect() {
    if (!hasSelection()) return;
    checkpoint("Deselect");
    m_lastSel = m_sel;
    setSelection(nullptr);
}

void Document::invertSelection() {
    checkpoint("Invert selection");
    auto m = std::make_shared<Mask>(std::size_t(m_w) * m_h, 255);
    if (hasSelection()) for (std::size_t i = 0; i < m->size(); ++i) (*m)[i] = std::uint8_t(255 - (*m_sel)[i]);
    setSelection(std::move(m));
}

void Document::selectLayerAlpha(SelOp op, int layer) {
    checkpoint("Select layer pixels");
    const Pixels& p = pixels(layer < 0 || layer >= layerCount() ? m_active : layer);
    Mask m(std::size_t(m_w) * m_h);
    for (std::size_t i = 0; i < m.size(); ++i) m[i] = p[i * 4 + 3];
    combineSelection(std::move(m), op);
}

// --- Undo -----------------------------------------------------------------------------

Document::State Document::capture() const {
    State s;
    s.w = m_w; s.h = m_h;
    s.layers = m_layers;
    s.active = m_active;
    s.sel = m_sel;
    s.selBounds = m_selBounds;
    s.version = m_version;
    return s;
}

void Document::restore(const State& s) {
    const bool reshape = s.w != m_w || s.h != m_h;
    m_layers = s.layers;
    m_active = std::clamp(s.active, 0, int(m_layers.size()) - 1);
    m_sel = s.sel;
    m_selBounds = s.selBounds;
    ++m_selVersion;
    m_version = s.version;
    if (reshape) reshaped(s.w, s.h);
    dirtyAll();
}

void Document::checkpoint(const std::string& label, const std::string& mergeKey) {
    if (m_editing) endEdit();
    if (!mergeKey.empty() && mergeKey == m_lastKey && !m_undo.empty()) {
        touched();
        return;
    }
    State s = capture();
    s.label = label;
    s.mergeKey = mergeKey;
    m_undo.push_back(std::move(s));
    m_redo.clear();
    m_lastKey = mergeKey;
    touched();
    trimUndo();
}

void Document::trimUndo() {
    // Keep at most 60 steps and about 1.5 GB of pixels nobody else holds.
    const std::size_t kSteps = 60, kBytes = std::size_t(1536) << 20;
    while (m_undo.size() > kSteps) m_undo.erase(m_undo.begin());
    for (;;) {
        std::unordered_set<const void*> seen;
        std::size_t bytes = 0;
        auto count = [&](const State& s) {
            for (const Layer& l : s.layers)
                if (seen.insert(l.px.get()).second) bytes += l.px->size();
        };
        for (const State& s : m_undo) count(s);
        for (const State& s : m_redo) count(s);
        if (bytes <= kBytes || m_undo.size() <= 2) break;
        m_undo.erase(m_undo.begin());
    }
}

bool Document::undo() {
    if (m_editing) endEdit();
    if (m_undo.empty()) return false;
    State cur = capture();
    cur.label = m_undo.back().label;
    State prev = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(std::move(cur));
    restore(prev);
    m_lastKey.clear();
    return true;
}

bool Document::redo() {
    if (m_editing) endEdit();
    if (m_redo.empty()) return false;
    State cur = capture();
    cur.label = m_redo.back().label;
    State next = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back(std::move(cur));
    restore(next);
    m_lastKey.clear();
    return true;
}

std::vector<std::string> Document::history(int& applied) const {
    std::vector<std::string> out;
    for (const State& s : m_undo) out.push_back(s.label);
    for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it) out.push_back(it->label);
    applied = int(m_undo.size());
    return out;
}

void Document::jumpTo(int applied) {
    while (int(m_undo.size()) > applied && undo()) {}
    while (int(m_undo.size()) < applied && redo()) {}
}

// --- Live edits -----------------------------------------------------------------------

void Document::beginEdit(const std::string& label) {
    if (m_editing) endEdit();
    checkpoint(label);
    m_base = m_layers[std::size_t(m_active)].px;
    m_baseSel = m_sel;
    m_editing = true;
}

void Document::restoreFromBase(const Rect& rIn) {
    const Rect r = rIn.clipped(m_w, m_h);
    if (!m_editing || r.empty()) return;
    std::uint8_t* d = pixelsMut(m_active);
    for (int y = r.y0; y < r.y1; ++y) {
        const std::size_t o = (std::size_t(y) * m_w + r.x0) * 4;
        std::memcpy(d + o, m_base->data() + o, std::size_t(r.w()) * 4);
    }
    dirty(r);
}

void Document::endEdit() {
    m_editing = false;
    m_base.reset();
    m_baseSel.reset();
}

void Document::cancelEdit() {
    if (!m_editing) return;
    m_editing = false;
    m_base.reset();
    m_baseSel.reset();
    if (m_undo.empty()) return;
    State s = std::move(m_undo.back());
    m_undo.pop_back();
    restore(s);
    m_lastKey.clear();
}

// --- Whole-picture operations ---------------------------------------------------------

namespace {

// Separable resample with a tent filter as wide as the scale: bilinear going up,
// an area average going down. Done on premultiplied colour, so a see-through
// pixel's hidden colour does not bleed a dark fringe into its neighbours.
Pixels resample(const Pixels& src, int sw, int sh, int dw, int dh) {
    std::vector<float> pm(std::size_t(sw) * sh * 4);
    for (std::size_t i = 0; i < pm.size(); i += 4) {
        const float a = src[i + 3] * px::k1_255;
        pm[i + 0] = src[i + 0] * px::k1_255 * a;
        pm[i + 1] = src[i + 1] * px::k1_255 * a;
        pm[i + 2] = src[i + 2] * px::k1_255 * a;
        pm[i + 3] = a;
    }
    auto pass = [](const std::vector<float>& in, int n, int m, int lines, bool horiz) {
        // n -> m samples along the axis, `lines` lines across it.
        std::vector<float> out(std::size_t(m) * lines * 4, 0.0f);
        const float scale = float(n) / float(m);
        const float support = std::max(1.0f, scale);
        for (int i = 0; i < m; ++i) {
            const float c = (i + 0.5f) * scale - 0.5f;
            const int a = int(std::floor(c - support)), b = int(std::ceil(c + support));
            float ws[64]; int idx[64]; int k = 0; float sum = 0.0f;
            for (int j = a; j <= b && k < 64; ++j) {
                const float wgt = std::max(0.0f, 1.0f - std::fabs(j - c) / support);
                if (wgt <= 0.0f) continue;
                ws[k] = wgt; idx[k] = std::clamp(j, 0, n - 1); sum += wgt; ++k;
            }
            if (sum <= 0.0f) { ws[0] = 1.0f; idx[0] = std::clamp(int(std::lround(c)), 0, n - 1); k = 1; sum = 1.0f; }
            for (int t = 0; t < k; ++t) ws[t] /= sum;
            for (int l = 0; l < lines; ++l) {
                float acc[4] = {0, 0, 0, 0};
                for (int t = 0; t < k; ++t) {
                    const std::size_t s = horiz ? (std::size_t(l) * n + idx[t]) * 4 : (std::size_t(idx[t]) * lines + l) * 4;
                    for (int ch = 0; ch < 4; ++ch) acc[ch] += in[s + ch] * ws[t];
                }
                const std::size_t d = horiz ? (std::size_t(l) * m + i) * 4 : (std::size_t(i) * lines + l) * 4;
                for (int ch = 0; ch < 4; ++ch) out[d + ch] = acc[ch];
            }
        }
        return out;
    };
    std::vector<float> h = pass(pm, sw, dw, sh, true);     // dw x sh
    std::vector<float> v = pass(h, sh, dh, dw, false);     // dw x dh
    Pixels out(std::size_t(dw) * dh * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        const float a = v[i + 3];
        const float inv = a > 1e-6f ? 1.0f / a : 0.0f;
        out[i + 0] = px::to8(v[i + 0] * inv);
        out[i + 1] = px::to8(v[i + 1] * inv);
        out[i + 2] = px::to8(v[i + 2] * inv);
        out[i + 3] = px::to8(a);
    }
    return out;
}

// Every pixel of a w x h image moved by a mapping to a new nw x nh image.
template <class Map>
Pixels remap(const Pixels& src, int w, int nw, int nh, int bpp, Map map) {
    Pixels out(std::size_t(nw) * nh * bpp, 0);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            int sx, sy;
            if (!map(x, y, sx, sy)) continue;
            std::memcpy(&out[(std::size_t(y) * nw + x) * bpp], &src[(std::size_t(sy) * w + sx) * bpp], std::size_t(bpp));
        }
    return out;
}

} // namespace

void Document::resize(int w, int h) {
    w = std::clamp(w, 1, 16384); h = std::clamp(h, 1, 16384);
    if (w == m_w && h == m_h) return;
    checkpoint("Image size");
    for (Layer& l : m_layers) l.px = std::make_shared<Pixels>(resample(*l.px, m_w, m_h, w, h));
    setSelection(nullptr);
    reshaped(w, h);
}

void Document::canvasSize(int w, int h, int ax, int ay) {
    w = std::clamp(w, 1, 16384); h = std::clamp(h, 1, 16384);
    if (w == m_w && h == m_h) return;
    checkpoint("Canvas size");
    const int ox = (w - m_w) * ax / 2, oy = (h - m_h) * ay / 2;
    const int ow = m_w, oh = m_h;
    auto map = [&](int x, int y, int& sx, int& sy) {
        sx = x - ox; sy = y - oy;
        return sx >= 0 && sy >= 0 && sx < ow && sy < oh;
    };
    for (Layer& l : m_layers) l.px = std::make_shared<Pixels>(remap(*l.px, ow, w, h, 4, map));
    if (hasSelection()) setSelection(std::make_shared<Mask>(remap(*m_sel, ow, w, h, 1, map)));
    reshaped(w, h);
}

void Document::crop(const Rect& rIn) {
    const Rect r = rIn.clipped(m_w, m_h);
    if (r.empty() || (r.w() == m_w && r.h() == m_h)) return;
    checkpoint("Crop");
    const int ow = m_w;
    auto map = [&](int x, int y, int& sx, int& sy) { sx = x + r.x0; sy = y + r.y0; return true; };
    for (Layer& l : m_layers) l.px = std::make_shared<Pixels>(remap(*l.px, ow, r.w(), r.h(), 4, map));
    setSelection(nullptr);
    reshaped(r.w(), r.h());
}

void Document::trim() {
    const Pixels flat = flattened();
    int x0 = m_w, y0 = m_h, x1 = -1, y1 = -1;
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x)
            if (flat[(std::size_t(y) * m_w + x) * 4 + 3]) {
                x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 < 0) return;
    crop({x0, y0, x1 + 1, y1 + 1});
}

void Document::rotate90(bool cw) {
    checkpoint(cw ? "Rotate 90 right" : "Rotate 90 left");
    const int ow = m_w, oh = m_h;
    auto map = [&](int x, int y, int& sx, int& sy) {
        if (cw) { sx = y; sy = oh - 1 - x; } else { sx = ow - 1 - y; sy = x; }
        return true;
    };
    for (Layer& l : m_layers) l.px = std::make_shared<Pixels>(remap(*l.px, ow, oh, ow, 4, map));
    if (hasSelection()) setSelection(std::make_shared<Mask>(remap(*m_sel, ow, oh, ow, 1, map)));
    reshaped(oh, ow);
}

void Document::rotate180() {
    checkpoint("Rotate 180");
    const int w = m_w, h = m_h;
    auto map = [&](int x, int y, int& sx, int& sy) { sx = w - 1 - x; sy = h - 1 - y; return true; };
    for (Layer& l : m_layers) l.px = std::make_shared<Pixels>(remap(*l.px, w, w, h, 4, map));
    if (hasSelection()) setSelection(std::make_shared<Mask>(remap(*m_sel, w, w, h, 1, map)));
    dirtyAll();
}

void Document::flipImage(bool horiz) {
    checkpoint(horiz ? "Flip horizontal" : "Flip vertical");
    const int w = m_w, h = m_h;
    auto map = [&](int x, int y, int& sx, int& sy) {
        sx = horiz ? w - 1 - x : x; sy = horiz ? y : h - 1 - y; return true;
    };
    for (Layer& l : m_layers) l.px = std::make_shared<Pixels>(remap(*l.px, w, w, h, 4, map));
    if (hasSelection()) setSelection(std::make_shared<Mask>(remap(*m_sel, w, w, h, 1, map)));
    dirtyAll();
}

void Document::flipLayer(bool horiz) {
    checkpoint(horiz ? "Flip layer horizontal" : "Flip layer vertical");
    const int w = m_w, h = m_h;
    auto map = [&](int x, int y, int& sx, int& sy) {
        sx = horiz ? w - 1 - x : x; sy = horiz ? y : h - 1 - y; return true;
    };
    m_layers[std::size_t(m_active)].px = std::make_shared<Pixels>(remap(pixels(m_active), w, w, h, 4, map));
    dirtyAll();
}

void Document::moveSelected(int dx, int dy) {
    const bool own = !m_editing;
    if (own) beginEdit("Move");
    const Pixels& base = *m_base;
    std::uint8_t* d = pixelsMut(m_active);
    const int w = m_w, h = m_h;
    if (!m_baseSel) {
        std::memset(d, 0, base.size());
        for (int y = 0; y < h; ++y) {
            const int sy = y - dy;
            if (sy < 0 || sy >= h) continue;
            const int x0 = std::max(0, dx), x1 = std::min(w, w + dx);
            if (x1 <= x0) continue;
            std::memcpy(d + (std::size_t(y) * w + x0) * 4, base.data() + (std::size_t(sy) * w + x0 - dx) * 4,
                        std::size_t(x1 - x0) * 4);
        }
    } else {
        // Lift what is selected out of the layer, set it down (dx, dy) further on.
        const Mask& s = *m_baseSel;
        std::memcpy(d, base.data(), base.size());
        for (std::size_t i = 0; i < s.size(); ++i)
            if (s[i]) d[i * 4 + 3] = std::uint8_t(d[i * 4 + 3] * (255 - s[i]) / 255);
        auto moved = std::make_shared<Mask>(s.size(), 0);
        for (int y = 0; y < h; ++y) {
            const int ty = y + dy;
            if (ty < 0 || ty >= h) continue;
            for (int x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * w + x;
                if (!s[i]) continue;
                const int tx = x + dx;
                if (tx < 0 || tx >= w) continue;
                const std::size_t t = std::size_t(ty) * w + tx;
                (*moved)[t] = s[i];
                const std::uint8_t* p = base.data() + i * 4;
                px::over(d + t * 4, p[0] * px::k1_255, p[1] * px::k1_255, p[2] * px::k1_255,
                         p[3] * px::k1_255 * s[i] * px::k1_255);
            }
        }
        setSelection(std::move(moved));
    }
    dirtyAll();
    if (own) endEdit();
}

Rect Document::transformBounds() const {
    if (hasSelection()) return m_selBounds;
    const Pixels& p = pixels(m_active);
    int x0 = m_w, y0 = m_h, x1 = -1, y1 = -1;
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x)
            if (p[(std::size_t(y) * m_w + x) * 4 + 3]) {
                x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 < 0) return {0, 0, m_w, m_h};
    return {x0, y0, x1 + 1, y1 + 1};
}

void Document::transformSelected(const Xform& xf) {
    const bool own = !m_editing;
    if (own) beginEdit("Free transform");
    const Pixels& base = *m_base;
    const Mask* bs = m_baseSel.get();
    std::uint8_t* d = pixelsMut(m_active);
    const int w = m_w, h = m_h;
    const std::size_t n = std::size_t(w) * h;
    // What stays behind: the layer without what is lifted.
    if (!bs) std::memset(d, 0, base.size());
    else {
        std::memcpy(d, base.data(), base.size());
        for (std::size_t i = 0; i < n; ++i)
            if ((*bs)[i]) d[i * 4 + 3] = std::uint8_t(d[i * 4 + 3] * (255 - (*bs)[i]) / 255);
    }
    Rect src{0, 0, w, h};
    if (bs) {
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if ((*bs)[std::size_t(y) * w + x]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
        src = x1 < 0 ? Rect{} : Rect{x0, y0, x1 + 1, y1 + 1};
    }
    std::shared_ptr<Mask> moved = bs ? std::make_shared<Mask>(n, 0) : nullptr;
    if (!src.empty()) {
        float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
        for (int k = 0; k < 4; ++k) {
            float ox, oy;
            xf.apply(float(k & 1 ? src.x1 : src.x0), float(k & 2 ? src.y1 : src.y0), ox, oy);
            lx = std::min(lx, ox); ly = std::min(ly, oy); hx = std::max(hx, ox); hy = std::max(hy, oy);
        }
        const Rect dst = Rect{int(std::floor(lx)) - 1, int(std::floor(ly)) - 1, int(std::ceil(hx)) + 1, int(std::ceil(hy)) + 1}.clipped(w, h);
        const float a = xf.rot * 0.017453293f, c = std::cos(a), s = std::sin(a);
        const float isx = 1.0f / (std::fabs(xf.sx) < 1e-3f ? (xf.sx < 0 ? -1e-3f : 1e-3f) : xf.sx);
        const float isy = 1.0f / (std::fabs(xf.sy) < 1e-3f ? (xf.sy < 0 ? -1e-3f : 1e-3f) : xf.sy);
        for (int y = dst.y0; y < dst.y1; ++y)
            for (int x = dst.x0; x < dst.x1; ++x) {
                const float qx = x + 0.5f - xf.px - xf.tx, qy = y + 0.5f - xf.py - xf.ty;
                const float u = (c * qx + s * qy) * isx + xf.px - 0.5f;
                const float v = (-s * qx + c * qy) * isy + xf.py - 0.5f;
                const int u0 = int(std::floor(u)), v0 = int(std::floor(v));
                const float fu = u - u0, fv = v - v0;
                float acc[4] = {0, 0, 0, 0}, ms = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const int sx = u0 + (k & 1), sy = v0 + (k >> 1);
                    if (sx < src.x0 || sy < src.y0 || sx >= src.x1 || sy >= src.y1) continue;
                    const std::size_t si = std::size_t(sy) * w + sx;
                    const float sel = bs ? (*bs)[si] * px::k1_255 : 1.0f;
                    if (sel <= 0.0f) continue;
                    const float wgt = ((k & 1) ? fu : 1.0f - fu) * ((k >> 1) ? fv : 1.0f - fv) * sel;
                    const std::uint8_t* p = &base[si * 4];
                    const float al = p[3] * px::k1_255 * wgt;
                    acc[0] += p[0] * px::k1_255 * al; acc[1] += p[1] * px::k1_255 * al;
                    acc[2] += p[2] * px::k1_255 * al; acc[3] += al;
                    ms += wgt;
                }
                const std::size_t di = std::size_t(y) * w + x;
                if (moved) (*moved)[di] = px::to8(ms);
                if (acc[3] > 1e-5f) px::over(d + di * 4, acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], acc[3]);
            }
    }
    if (moved) setSelection(std::move(moved));
    dirtyAll();
    if (own) endEdit();
}

void Document::offsetLayer(int dx, int dy) {
    checkpoint("Offset layer");
    const int w = m_w, h = m_h;
    auto map = [&](int x, int y, int& sx, int& sy) {
        sx = ((x - dx) % w + w) % w; sy = ((y - dy) % h + h) % h; return true;
    };
    m_layers[std::size_t(m_active)].px = std::make_shared<Pixels>(remap(pixels(m_active), w, w, h, 4, map));
    dirtyAll();
}

void Document::fillSelection(Color c) {
    checkpoint("Fill");
    const bool lock = m_layers[std::size_t(m_active)].lockAlpha;
    std::uint8_t* d = pixelsMut(m_active);
    const std::size_t n = std::size_t(m_w) * m_h;
    for (std::size_t i = 0; i < n; ++i) {
        const int s = selAt(i);
        if (s) px::paint(d + i * 4, c.r, c.g, c.b, c.a * s * px::k1_255, lock);
    }
    dirtyAll();
}

void Document::clearSelection() {
    checkpoint("Clear");
    std::uint8_t* d = pixelsMut(m_active);
    const std::size_t n = std::size_t(m_w) * m_h;
    for (std::size_t i = 0; i < n; ++i) {
        const int s = selAt(i);
        if (s) d[i * 4 + 3] = std::uint8_t(d[i * 4 + 3] * (255 - s) / 255);
    }
    dirtyAll();
}

void Document::floodFill(int x, int y, Color c, float tol, bool contiguous, bool merged, float opacity) {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return;
    const Pixels src = merged ? flattened() : pixels(m_active);
    const Mask m = colourRegion(src, m_w, m_h, x, y, tol, contiguous);
    checkpoint("Paint bucket");
    const bool lock = m_layers[std::size_t(m_active)].lockAlpha;
    std::uint8_t* d = pixelsMut(m_active);
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (!m[i]) continue;
        const int s = selAt(i);
        if (s) px::paint(d + i * 4, c.r, c.g, c.b, c.a * opacity * s * px::k1_255, lock);
    }
    dirtyAll();
}

template <class Fn>
void Document::rewrite(const std::string& label, Fn fn) {
    if (!m_editing) checkpoint(label);
    // The source stays alive (and unshared with the result) while fn reads it.
    const std::shared_ptr<Pixels> src = m_editing ? m_base : m_layers[std::size_t(m_active)].px;
    Pixels out;
    fn(*src, out);
    if (m_layers[std::size_t(m_active)].lockAlpha && out.size() == src->size())
        for (std::size_t i = 3; i < out.size(); i += 4) out[i] = (*src)[i];
    m_layers[std::size_t(m_active)].px = std::make_shared<Pixels>(std::move(out));
    dirtyAll();
}

void Document::filter(Filter f, const float* params, std::uint32_t seed, const Color* colors) {
    rewrite(filterInfo(f).name, [&](const Pixels& src, Pixels& out) {
        applyFilter(f, params, src, out, m_w, m_h, selection(), seed, colors);
    });
}

void Document::curves(const Curves& c) {
    rewrite("Curves", [&](const Pixels& src, Pixels& out) { applyCurves(c, src, out, m_w, m_h, selection()); });
}

Clip Document::copy(bool merged) const {
    Clip c;
    const Rect r = hasSelection() ? m_selBounds : Rect{0, 0, m_w, m_h};
    if (r.empty()) return c;
    const Pixels src = merged ? flattened() : pixels(m_active);
    c.x = r.x0; c.y = r.y0; c.w = r.w(); c.h = r.h();
    c.px.assign(std::size_t(c.w) * c.h * 4, 0);
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            const std::size_t i = std::size_t(y + r.y0) * m_w + (x + r.x0);
            std::uint8_t* d = &c.px[(std::size_t(y) * c.w + x) * 4];
            std::memcpy(d, &src[i * 4], 4);
            d[3] = std::uint8_t(d[3] * selAt(i) / 255);
        }
    return c;
}

void Document::paste(const Clip& c, const std::string& name) {
    if (!c.valid()) return;
    checkpoint("Paste");
    int x = c.x, y = c.y;
    // Somewhere it can be seen: where it came from if that is on this canvas,
    // else the middle.
    if (x + c.w <= 0 || y + c.h <= 0 || x >= m_w || y >= m_h) { x = (m_w - c.w) / 2; y = (m_h - c.h) / 2; }
    insertLayer(name, c.px, c.w, c.h, x, y);
}

void Document::insertLayer(const std::string& name, const Pixels& p, int w, int h, int x, int y) {
    if (m_editing) endEdit();
    Layer l;
    l.name = name;
    l.px = std::make_shared<Pixels>(std::size_t(m_w) * m_h * 4, std::uint8_t(0));
    m_layers.insert(m_layers.begin() + m_active + 1, std::move(l));
    ++m_active;
    placeOnLayer(m_active, p, w, h, x, y);
}

void Document::placeOnLayer(int i, const Pixels& p, int w, int h, int x, int y) {
    std::uint8_t* d = pixelsMut(i);
    std::memset(d, 0, std::size_t(m_w) * m_h * 4);
    for (int sy = 0; sy < h; ++sy) {
        const int ty = y + sy;
        if (ty < 0 || ty >= m_h) continue;
        const int sx0 = std::max(0, -x), sx1 = std::min(w, m_w - x);
        if (sx1 <= sx0) continue;
        std::memcpy(d + (std::size_t(ty) * m_w + x + sx0) * 4, p.data() + (std::size_t(sy) * w + sx0) * 4,
                    std::size_t(sx1 - sx0) * 4);
    }
    dirtyAll();
}

} // namespace img
