// The Image editor's canvas: the picture on screen (a GL texture kept in step
// with the document's dirty rectangles), pan and zoom, and what each tool does
// with the pointer -- including the live gestures that are more than one click:
// the lasso, the crop box, free transform. The window around it is
// ImageEditPanel.cpp.
#include "ImageEditPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glad/gl.h>
#include <imgui.h>

#include "ImageIcons.hpp"

namespace imageui {

namespace {

ImVec2 add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 mul(ImVec2 a, float k) { return {a.x * k, a.y * k}; }
float  len(ImVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

const ImU32 kInk   = IM_COL32(0, 0, 0, 220);
const ImU32 kPaper = IM_COL32(255, 255, 255, 235);

// A line that reads on any picture: dark under, light over.
void line2(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddLine(a, b, kInk, 3.0f);
    dl->AddLine(a, b, kPaper, 1.0f);
}

void circle2(ImDrawList* dl, ImVec2 c, float r) {
    dl->AddCircle(c, r, kInk, 0, 3.0f);
    dl->AddCircle(c, r, kPaper, 0, 1.0f);
}

void ellipse2(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const ImVec2 c = mul(add(a, b), 0.5f);
    const float rx = std::fabs(b.x - a.x) * 0.5f, ry = std::fabs(b.y - a.y) * 0.5f;
    ImVec2 pts[72];
    for (int i = 0; i < 72; ++i) {
        const float t = float(i) / 72.0f * 6.2831853f;
        pts[i] = {c.x + std::cos(t) * rx, c.y + std::sin(t) * ry};
    }
    dl->AddPolyline(pts, 72, kInk, ImDrawFlags_Closed, 3.0f);
    dl->AddPolyline(pts, 72, kPaper, ImDrawFlags_Closed, 1.0f);
}

void rect2(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const ImVec2 lo{std::min(a.x, b.x), std::min(a.y, b.y)}, hi{std::max(a.x, b.x), std::max(a.y, b.y)};
    dl->AddRect(lo, hi, kInk, 0.0f, 0, 3.0f);
    dl->AddRect(lo, hi, kPaper, 0.0f, 0, 1.0f);
}

void handle2(ImDrawList* dl, ImVec2 c, float r = 4.5f) {
    dl->AddRectFilled(ImVec2(c.x - r - 1, c.y - r - 1), ImVec2(c.x + r + 1, c.y + r + 1), kInk);
    dl->AddRectFilled(ImVec2(c.x - r + 0.5f, c.y - r + 0.5f), ImVec2(c.x + r - 0.5f, c.y + r - 0.5f), kPaper);
}

bool insideQuad(const ImVec2 q[4], ImVec2 p) {
    int pos = 0, neg = 0;
    for (int i = 0; i < 4; ++i) {
        const ImVec2 a = q[i], b = q[(i + 1) % 4];
        const float c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (c > 0) ++pos; else if (c < 0) ++neg;
    }
    return pos == 0 || neg == 0;
}

// The eight handles of a box, clockwise from the top left corner, as fractions of it.
const float kHandle[8][2] = {{0, 0}, {0.5f, 0}, {1, 0}, {1, 0.5f}, {1, 1}, {0.5f, 1}, {0, 1}, {0, 0.5f}};

const float kZooms[] = {1.0f / 32, 1.0f / 16, 1.0f / 8, 1.0f / 6, 0.25f, 1.0f / 3, 0.5f, 2.0f / 3, 1.0f,
                        1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f};

} // namespace

// The GL backend binds sampler objects of its own (linear or nearest, both
// clamped), and a sampler wins over a texture's own settings. The picture wants
// its texture's: magnified pixels stay sharp squares, and the checks under it
// repeat. So the canvas lets go of the sampler while it draws, and asks for the
// backend's usual one back afterwards.
static void useTextureSampling(const ImDrawList*, const ImDrawCmd*) { glBindSampler(0, 0); }

void textureSampling(ImDrawList* dl, bool own) {
    if (own) { dl->AddCallback(useTextureSampling, nullptr); return; }
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
    else if (pio.DrawCallback_ResetRenderState) dl->AddCallback(pio.DrawCallback_ResetRenderState, nullptr);
}

float nextZoom(float z, int dir) {
    if (dir > 0) { for (float s : kZooms) if (s > z * 1.001f) return s; return 64.0f; }
    for (int i = int(sizeof kZooms / sizeof kZooms[0]) - 1; i >= 0; --i) if (kZooms[i] < z * 0.999f) return kZooms[i];
    return kZooms[0];
}

// --- The texture ----------------------------------------------------------------------

void ImageEditor::upload(View& v) {
    img::Document& d = v.doc;
    const int w = d.width(), h = d.height();
    if (!v.tex || v.shape != d.shapeVersion()) {
        if (v.tex) m_retired.push_back(v.tex);   // a draw this frame may still name it
        glGenTextures(1, &v.tex);
        glBindTexture(GL_TEXTURE_2D, v.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // No mipmaps: this driver's glGenerateMipmap is not to be trusted, and a
        // picture being edited changes every frame anyway.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        v.comp.assign(std::size_t(w) * h * 4, 0);
        v.shape = d.shapeVersion();
        v.selVer = 0;
        d.dirtyAll();
    }
    const img::Rect r = d.takeDirty();
    if (r.empty()) return;
    d.composite(r, v.comp.data());
    glBindTexture(GL_TEXTURE_2D, v.tex);
    // Only the changed rectangle goes up; the pixel-store state is put back to
    // GL's defaults afterwards, since the rest of the editor assumes them.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, r.x0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, r.y0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, r.x0, r.y0, r.w(), r.h(), GL_RGBA, GL_UNSIGNED_BYTE, v.comp.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ImageEditor::traceOutline(View& v) {
    const img::Document& d = v.doc;
    v.selVer = d.selectionVersion();
    v.outline.clear();
    v.outlineBox = false;
    const img::Mask* m = d.selection();
    if (!m) return;
    const int w = d.width(), h = d.height();
    const img::Rect b = d.selectionBounds();
    auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && (*m)[std::size_t(y) * w + x] >= 128; };
    const std::size_t cap = 250000;
    for (int y = b.y0; y <= b.y1 && v.outline.size() < cap; ++y) {
        int run = -1;
        for (int x = b.x0; x <= b.x1; ++x) {
            const bool e = x < b.x1 && in(x, y - 1) != in(x, y);
            if (e && run < 0) run = x;
            if (!e && run >= 0) { v.outline.push_back(ImVec4(float(run), float(y), float(x), float(y))); run = -1; }
        }
    }
    for (int x = b.x0; x <= b.x1 && v.outline.size() < cap; ++x) {
        int run = -1;
        for (int y = b.y0; y <= b.y1; ++y) {
            const bool e = y < b.y1 && in(x - 1, y) != in(x, y);
            if (e && run < 0) run = y;
            if (!e && run >= 0) { v.outline.push_back(ImVec4(float(x), float(run), float(x), float(y))); run = -1; }
        }
    }
    if (v.outline.size() >= cap) { v.outline.clear(); v.outlineBox = true; }
}

// --- View -----------------------------------------------------------------------------

void ImageEditor::fit(View& v, ImVec2 size) {
    const float W = float(v.doc.width()), H = float(v.doc.height());
    float z = std::min((size.x - 24.0f) / W, (size.y - 24.0f) / H);
    z = std::clamp(z, 1.0f / 32.0f, 64.0f);
    if (z >= 1.0f) z = std::floor(z);   // whole pixels when it fits bigger than 1:1
    v.zoom = z;
    v.pan = {0.0f, 0.0f};
}

void ImageEditor::zoomAt(View& v, float z, ImVec2 screen) {
    z = std::clamp(z, 1.0f / 32.0f, 64.0f);
    const float W = float(v.doc.width()), H = float(v.doc.height());
    const ImVec2 origin = sub(add(m_center, v.pan), ImVec2(W * v.zoom * 0.5f, H * v.zoom * 0.5f));
    const ImVec2 at = mul(sub(screen, origin), 1.0f / v.zoom);   // the image point under the pointer stays
    const ImVec2 o2 = sub(screen, mul(at, z));
    v.pan = add(sub(o2, m_center), ImVec2(W * z * 0.5f, H * z * 0.5f));
    v.zoom = z;
}

void ImageEditor::setZoom(float z) {
    if (View* v = cur()) zoomAt(*v, z, m_center);
}

ImVec2 ImageEditor::toScreen(ImVec2 p) const {
    if (m_cur < 0 || m_cur >= int(m_views.size())) return p;
    const View& v = *m_views[std::size_t(m_cur)];
    const ImVec2 origin = sub(add(m_center, v.pan), ImVec2(v.doc.width() * v.zoom * 0.5f, v.doc.height() * v.zoom * 0.5f));
    return add(origin, mul(p, v.zoom));
}

ImVec2 ImageEditor::snapped(ImVec2 p) const {
    const float s = float(std::max(1, m_snap));
    return {std::round(p.x / s) * s, std::round(p.y / s) * s};
}

ImVec2 ImageEditor::constrained(ImVec2 a, ImVec2 p, bool box) const {
    if (!m_constrain && !ImGui::GetIO().KeyShift) return p;
    const ImVec2 d = sub(p, a);
    if (box) {
        const float m = std::max(std::fabs(d.x), std::fabs(d.y));
        return {a.x + (d.x < 0 ? -m : m), a.y + (d.y < 0 ? -m : m)};
    }
    const float l = len(d);
    if (l <= 0.0f) return p;
    const float step = 3.14159265f / 4.0f;
    const float ang = std::round(std::atan2(d.y, d.x) / step) * step;
    return {std::round(a.x + std::cos(ang) * l), std::round(a.y + std::sin(ang) * l)};
}

img::SelOp ImageEditor::gestureOp() const {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift && io.KeyAlt) return img::SelOp::Intersect;
    if (io.KeyShift) return img::SelOp::Add;
    if (io.KeyAlt) return img::SelOp::Subtract;
    return m_selOp;
}

// --- Text -----------------------------------------------------------------------------

float ImageEditor::textPad() const { return std::ceil(m_textSize * 0.15f) + 2.0f; }

void ImageEditor::textUpdate(View& v) {
    if (!m_text.on || m_text.viewId != v.id) return;
    img::Document& d = v.doc;
    // Anything else done to the picture meanwhile (undo, a new layer ...) ends it.
    if (d.layerCount() != m_text.layers || d.active() != m_text.layer) { m_text.on = false; return; }
    if (std::memcmp(&m_text.color, &m_fg, sizeof m_fg) != 0) { m_text.color = m_fg; m_text.dirty = true; }
    if (!m_text.dirty) return;
    m_text.dirty = false;
    if (m_font.empty()) {
        for (const auto& f : img::systemFonts())
            if (f.name == m_fontName) { m_font = f.path; break; }
        if (m_font.empty() && !img::systemFonts().empty()) {
            m_font = img::systemFonts().front().path;
            m_fontName = img::systemFonts().front().name;
        }
    }
    img::Pixels px;
    int w = 0, h = 0;
    const float pad = textPad();
    if (m_textBuf[0] && img::renderText(m_font, m_textBuf, m_textSize, m_fg, px, w, h))
        d.placeOnLayer(m_text.layer, px, w, h, int(m_text.pos.x - pad), int(m_text.pos.y - pad));
    else
        d.placeOnLayer(m_text.layer, {}, 0, 0, 0, 0);
}

void ImageEditor::textEnd(bool keep) {
    if (!m_text.on) return;
    m_text.on = false;
    for (auto& vp : m_views) {
        if (vp->id != m_text.viewId) continue;
        img::Document& d = vp->doc;
        if (d.layerCount() != m_text.layers || d.active() != m_text.layer) return;
        if (!keep || !m_textBuf[0]) {
            if (d.undoLabel() == "Text") d.undo();
            return;
        }
        std::string name = m_textBuf;
        std::replace(name.begin(), name.end(), '\n', ' ');
        if (name.size() > 28) name = name.substr(0, 26) + "..";
        d.rename(m_text.layer, name, false);
        rememberColor(m_fg);
    }
}

// --- Lasso, crop, free transform -------------------------------------------------------

void ImageEditor::lassoClose(View& v) {
    if (!m_lasso.on) return;
    std::vector<float> xy;
    for (const ImVec2& p : m_lasso.pts) { xy.push_back(p.x); xy.push_back(p.y); }
    const img::SelOp op = m_lasso.op;
    lassoCancel();
    if (xy.size() >= 6) v.doc.selectPolygon(xy, op);
    else if (op == img::SelOp::Replace) v.doc.deselect();
}

void ImageEditor::lassoCancel() {
    m_lasso.on = m_lasso.dragging = false;
    m_lasso.pts.clear();
}

void ImageEditor::cropCommit(View& v) {
    if (!m_crop.on) return;
    const img::Rect r{int(std::lround(std::min(m_crop.x0, m_crop.x1))), int(std::lround(std::min(m_crop.y0, m_crop.y1))),
                      int(std::lround(std::max(m_crop.x0, m_crop.x1))), int(std::lround(std::max(m_crop.y0, m_crop.y1)))};
    cropCancel();
    if (r.clipped(v.doc.width(), v.doc.height()).empty()) return;
    cancelGesture(v);
    v.doc.crop(r);
    v.fitPending = true;
}

void ImageEditor::cropCancel() {
    m_crop.on = false;
    m_crop.drag = -1;
}

void ImageEditor::startTransform(View& v) {
    if (m_xf.on) return;
    cancelGesture(v);
    textEnd(true);
    img::Document& d = v.doc;
    const img::Rect src = d.transformBounds();
    if (src.empty()) { m_d.status = "Nothing to transform on this layer."; return; }
    m_xf = {};
    m_xf.on = true;
    m_xf.viewId = v.id;
    m_xf.src = src;
    m_xf.xf.px = (src.x0 + src.x1) * 0.5f;
    m_xf.xf.py = (src.y0 + src.y1) * 0.5f;
    m_xf.start = m_xf.xf;
    d.beginEdit("Free transform");
}

void ImageEditor::transformApply(View& v) {
    if (!m_xf.on) return;
    if (!v.doc.editing()) { m_xf.on = false; return; }
    v.doc.transformSelected(m_xf.xf);
}

void ImageEditor::transformCommit(View& v) {
    if (!m_xf.on) return;
    m_xf.on = false;
    m_xf.drag = -1;
    if (!v.doc.editing()) return;
    if (m_xf.xf.identity()) v.doc.cancelEdit(); else v.doc.endEdit();
}

void ImageEditor::transformCancel(View& v) {
    if (!m_xf.on) return;
    m_xf.on = false;
    m_xf.drag = -1;
    if (v.doc.editing()) v.doc.cancelEdit();
}

void ImageEditor::transformCorners(ImVec2 out[4]) const {
    const img::Rect& r = m_xf.src;
    const float xs[4] = {float(r.x0), float(r.x1), float(r.x1), float(r.x0)};
    const float ys[4] = {float(r.y0), float(r.y0), float(r.y1), float(r.y1)};
    for (int k = 0; k < 4; ++k) m_xf.xf.apply(xs[k], ys[k], out[k].x, out[k].y);
}

int ImageEditor::transformHit(View& v, ImVec2 screen) const {
    (void)v;
    const img::Rect& r = m_xf.src;
    for (int h = 0; h < 8; ++h) {
        ImVec2 p;
        m_xf.xf.apply(r.x0 + r.w() * kHandle[h][0], r.y0 + r.h() * kHandle[h][1], p.x, p.y);
        if (len(sub(toScreen(p), screen)) <= 9.0f) return h;
    }
    ImVec2 q[4];
    transformCorners(q);
    for (ImVec2& p : q) p = toScreen(p);
    return insideQuad(q, screen) ? 8 : 9;
}

// --- Gestures -------------------------------------------------------------------------

void ImageEditor::startAnchor(View& v, ImVec2 p, ImVec2 screen) {
    img::Document& d = v.doc;
    m_anchorOn = true;
    m_anchorHeld = true;
    m_anchor = snapped(p);
    m_pressScreen = screen;
    m_lastQ = {-1e9f, -1e9f};
    m_anchorEdit = false;
    m_anchorOp = gestureOp();
    if (m_tool == Tool::Shape) {
        static const char* names[] = {"Line", "Rectangle", "Rectangle", "Ellipse", "Ellipse"};
        m_stroke.begin(d, img::PaintMode::Paint, m_fg, m_brush, 0, 0, names[std::clamp(m_shape, 0, 4)]);
        m_anchorEdit = true;
    } else if (m_tool == Tool::Gradient) {
        d.beginEdit("Gradient");
        m_anchorEdit = true;
    }
}

void ImageEditor::pointerDown(View& v, ImVec2 p, ImVec2 screen, bool right) {
    img::Document& d = v.doc;
    const ImGuiIO& io = ImGui::GetIO();
    const int ix = int(std::floor(p.x)), iy = int(std::floor(p.y));
    const bool inside = ix >= 0 && iy >= 0 && ix < d.width() && iy < d.height();
    if (right) {
        if (m_tool == Tool::Zoom) { zoomAt(v, nextZoom(v.zoom, -1), screen); return; }
        // A right click picks the colour under it -- the eyedropper without
        // changing tools (the Eyedropper tool itself: into the background colour).
        if (!inside) return;
        const img::Color c = d.sample(ix, iy, true);
        if (m_tool == Tool::Eyedropper) m_bg = c; else m_fg = c;
        return;
    }
    // Alt with a painting tool is the eyedropper, with the clone stamp the
    // source, with the zoom tool zooming out -- as in Photoshop.
    if (io.KeyAlt) {
        if (m_tool == Tool::Zoom) { zoomAt(v, nextZoom(v.zoom, -1), screen); return; }
        if (m_tool == Tool::Clone) {
            m_cloneSrc = p;
            m_cloneHasSrc = true;
            m_clonePick = false;
            m_cloneAligned = false;
            m_d.status = "Clone source set -- now paint where the copy should go.";
            return;
        }
        if ((m_tool == Tool::Brush || m_tool == Tool::Fill || m_tool == Tool::Gradient || m_tool == Tool::Shape) && inside) {
            m_fg = d.sample(ix, iy, m_sampleMerged);
            return;
        }
    }
    // A transform in progress takes the pointer whatever the tool.
    if (m_xf.on && m_xf.viewId == v.id) {
        m_xf.drag = transformHit(v, screen);
        m_xf.press = p;
        m_xf.start = m_xf.xf;
        const ImVec2 c(m_xf.xf.px + m_xf.xf.tx, m_xf.xf.py + m_xf.xf.ty);
        m_xf.pressAngle = std::atan2(p.y - c.y, p.x - c.x);
        return;
    }
    switch (m_tool) {
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Clone:
    case Tool::Blur:
    case Tool::Dodge: {
        if (m_tool == Tool::Clone && (m_clonePick || !m_cloneHasSrc)) {
            m_cloneSrc = p;
            m_cloneHasSrc = true;
            m_clonePick = false;
            m_cloneAligned = false;
            m_d.status = "Clone source set -- now paint where the copy should go.";
            return;
        }
        static const img::PaintMode tones[] = {img::PaintMode::Dodge, img::PaintMode::Burn, img::PaintMode::Sponge,
                                               img::PaintMode::Saturate};
        const img::PaintMode mode = m_tool == Tool::Eraser ? img::PaintMode::Erase
                                  : m_tool == Tool::Clone  ? img::PaintMode::Clone
                                  : m_tool == Tool::Blur   ? (m_sharpen ? img::PaintMode::Sharpen : img::PaintMode::Blur)
                                  : m_tool == Tool::Dodge  ? tones[std::clamp(m_tone, 0, 3)] : img::PaintMode::Paint;
        img::Brush b = m_brush;
        if (m_tool == Tool::Blur || m_tool == Tool::Dodge) b.opacity = m_retouch;
        if (m_tool == Tool::Clone && !m_cloneAligned) {
            m_cloneOff = sub(m_cloneSrc, p);
            m_cloneAligned = true;
        }
        const ImVec2 off = m_tool == Tool::Clone ? m_cloneOff : ImVec2(0, 0);
        // Line mode, or Shift+click after a stroke: a straight line, no drag.
        const bool shiftLine = io.KeyShift && v.hasLastPaint && !m_lineMode;
        if (m_lineMode || shiftLine) {
            const ImVec2 from = shiftLine ? v.lastPaint : m_lineLast;
            const bool has = shiftLine || m_lineHas;
            const ImVec2 q = has ? constrained(from, p, false) : p;
            m_stroke.begin(d, mode, m_fg, b, off.x, off.y);
            if (has) m_stroke.to(from.x, from.y);
            m_stroke.to(q.x, q.y);
            m_stroke.end();
            m_lineLast = q;
            m_lineHas = true;
            v.lastPaint = q;
            v.hasLastPaint = true;
            if (mode == img::PaintMode::Paint) rememberColor(m_fg);
            return;
        }
        m_stroke.begin(d, mode, m_fg, b, off.x, off.y);
        m_stroke.to(p.x, p.y);
        m_lazy = p;
        m_painting = true;
        return;
    }
    case Tool::Select:
        if (!m_anchorOn && d.hasSelection() && inside && d.selAt(std::size_t(iy) * d.width() + ix) >= 128 &&
            gestureOp() == img::SelOp::Replace) {
            // Inside the selection: a drag moves its outline, a click starts a new one.
            m_selPending = true;
            m_selMoving = false;
            m_selPress = p;
            m_selPressScreen = screen;
            return;
        }
        [[fallthrough]];
    case Tool::Shape:
    case Tool::Gradient:
        if (!m_anchorOn) startAnchor(v, p, screen);
        else finishAnchor(v, p);
        return;
    case Tool::Crop:
        if (m_crop.on && m_crop.viewId == v.id) {
            // On a handle or inside the box: adjust it. Elsewhere: a new box.
            const float x0 = std::min(m_crop.x0, m_crop.x1), y0 = std::min(m_crop.y0, m_crop.y1);
            const float x1 = std::max(m_crop.x0, m_crop.x1), y1 = std::max(m_crop.y0, m_crop.y1);
            m_crop.x0 = x0; m_crop.y0 = y0; m_crop.x1 = x1; m_crop.y1 = y1;
            m_crop.drag = -1;
            for (int h = 0; h < 8; ++h) {
                const ImVec2 hp = toScreen(ImVec2(x0 + (x1 - x0) * kHandle[h][0], y0 + (y1 - y0) * kHandle[h][1]));
                if (len(sub(hp, screen)) <= 9.0f) m_crop.drag = h;
            }
            if (m_crop.drag < 0 && p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1) m_crop.drag = 8;
            if (m_crop.drag >= 0) {
                m_crop.sx0 = x0; m_crop.sy0 = y0; m_crop.sx1 = x1; m_crop.sy1 = y1;
                m_crop.press = p;
                return;
            }
            cropCancel();
        }
        if (!m_anchorOn) startAnchor(v, p, screen);
        else finishAnchor(v, p);
        return;
    case Tool::Lasso:
        if (m_polyLasso) {
            if (!m_lasso.on || m_lasso.viewId != v.id) {
                lassoCancel();
                m_lasso.on = true;
                m_lasso.viewId = v.id;
                m_lasso.op = gestureOp();
                m_lasso.pts = {p};
                return;
            }
            const bool nearStart = m_lasso.pts.size() >= 3 && len(sub(toScreen(m_lasso.pts.front()), screen)) <= 9.0f;
            if (nearStart || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { lassoClose(v); return; }
            m_lasso.pts.push_back(p);
            return;
        }
        lassoCancel();
        m_lasso.on = m_lasso.dragging = true;
        m_lasso.viewId = v.id;
        m_lasso.op = gestureOp();
        m_lasso.pts = {p};
        return;
    case Tool::Wand:
        d.selectColor(ix, iy, m_tolerance, m_contiguous, m_sampleMerged, gestureOp());
        return;
    case Tool::Fill:
        if (!inside) return;
        d.floodFill(ix, iy, m_fg, m_tolerance, m_contiguous, m_sampleMerged, m_fillOpacity);
        rememberColor(m_fg);
        return;
    case Tool::Eyedropper:
        if (!inside) return;
        m_fg = d.sample(ix, iy, m_sampleMerged);
        return;
    case Tool::Zoom:
        zoomAt(v, nextZoom(v.zoom, +1), screen);
        return;
    case Tool::Move:
        if (m_autoSelect && inside) {
            for (int i = d.layerCount() - 1; i >= 0; --i)
                if (d.layer(i).visible && d.pixels(i)[(std::size_t(iy) * d.width() + ix) * 4 + 3] > 0) {
                    selectLayer(v, i);
                    break;
                }
        }
        d.beginEdit("Move");
        m_moving = true;
        m_moveStart = p;
        m_moveDx = m_moveDy = 0;
        return;
    case Tool::Text:
        if (m_text.on && m_text.viewId == v.id) {
            m_text.pos = snapped(p);     // another click moves the text there
            m_text.dirty = true;
            return;
        }
        textEnd(true);
        m_textBuf[0] = '\0';   // a new text starts empty; the field has the keys
        d.checkpoint("Text");
        d.insertLayer("Text", {}, 0, 0, 0, 0);
        m_text.on = true;
        m_text.viewId = v.id;
        m_text.layer = d.active();
        m_text.layers = d.layerCount();
        m_text.pos = snapped(p);
        m_text.color = m_fg;
        m_text.dirty = true;
        m_focusText = true;
        return;
    default:
        return;
    }
}

void ImageEditor::pointerDrag(View& v, ImVec2 p, ImVec2 screen) {
    img::Document& d = v.doc;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool shift = ImGui::GetIO().KeyShift || m_constrain;
    if (m_painting && down) {
        // The stabiliser: the brush is pulled along on a string, so the tremor
        // inside the string's length never reaches the picture.
        const float L = m_stabilize / v.zoom;
        const ImVec2 dv = sub(p, m_lazy);
        const float dist = len(dv);
        if (dist > L && dist > 0.0f) {
            m_lazy = add(m_lazy, mul(dv, (dist - L) / dist));
            m_stroke.to(m_lazy.x, m_lazy.y);
        }
    }
    if (m_moving && down) {
        const float s = float(std::max(1, m_snap));
        int dx = int(std::round((p.x - m_moveStart.x) / s) * s);
        int dy = int(std::round((p.y - m_moveStart.y) / s) * s);
        if (shift) { if (std::abs(dx) > std::abs(dy)) dy = 0; else dx = 0; }
        if (dx != m_moveDx || dy != m_moveDy) {
            m_moveDx = dx; m_moveDy = dy;
            d.moveSelected(dx, dy);
        }
    }
    if (m_selPending && down) {
        const float th = std::max(6.0f, ImGui::GetIO().MouseDragThreshold);
        if (!m_selMoving && len(sub(screen, m_selPressScreen)) > th) {
            m_selMoving = true;
            m_selBase = d.selectionShared();
            d.checkpoint("Move selection");
        }
        if (m_selMoving) {
            int dx = int(std::round(p.x - m_selPress.x)), dy = int(std::round(p.y - m_selPress.y));
            if (shift) { if (std::abs(dx) > std::abs(dy)) dy = 0; else dx = 0; }
            d.shiftSelection(m_selBase, dx, dy);
        }
    }
    if (m_lasso.on && m_lasso.dragging && down) {
        if (len(sub(toScreen(m_lasso.pts.back()), screen)) >= 2.0f) m_lasso.pts.push_back(p);
    }
    if (m_xf.on && m_xf.drag >= 0 && down) {
        img::Xform& x = m_xf.xf;
        const img::Xform& st = m_xf.start;
        const img::Rect& r = m_xf.src;
        if (m_xf.drag == 8) {
            ImVec2 dd = sub(p, m_xf.press);
            if (shift) { if (std::fabs(dd.x) > std::fabs(dd.y)) dd.y = 0; else dd.x = 0; }
            const float s = float(std::max(1, m_snap));
            x.tx = st.tx + std::round(dd.x / s) * s;
            x.ty = st.ty + std::round(dd.y / s) * s;
        } else if (m_xf.drag == 9) {
            const ImVec2 c(st.px + st.tx, st.py + st.ty);
            float a = st.rot + (std::atan2(p.y - c.y, p.x - c.x) - m_xf.pressAngle) * 57.29578f;
            if (shift) a = std::round(a / 15.0f) * 15.0f;
            while (a > 180.0f) a -= 360.0f;
            while (a < -180.0f) a += 360.0f;
            x.rot = a;
        } else {
            // Scale with the opposite handle held where it is.
            const int h = m_xf.drag, o = (h + 4) % 8;
            const float hx = r.x0 + r.w() * kHandle[h][0], hy = r.y0 + r.h() * kHandle[h][1];
            const float ox = r.x0 + r.w() * kHandle[o][0], oy = r.y0 + r.h() * kHandle[o][1];
            const float a = st.rot * 0.017453293f, c = std::cos(a), s = std::sin(a);
            const float qx = p.x - st.px - st.tx, qy = p.y - st.py - st.ty;
            const float lx = c * qx + s * qy, ly = -s * qx + c * qy;          // the pointer, unturned
            const float lox = st.sx * (ox - st.px), loy = st.sy * (oy - st.py);
            float sx = st.sx, sy = st.sy;
            if (std::fabs(hx - ox) > 0.5f) sx = (lx - lox) / (hx - ox);
            if (std::fabs(hy - oy) > 0.5f) sy = (ly - loy) / (hy - oy);
            const bool corner = h % 2 == 0;
            if ((shift || m_xf.keepAspect) && st.sx != 0.0f && st.sy != 0.0f) {
                const float rx = sx / st.sx, ry = sy / st.sy;
                const float k = corner ? (std::fabs(rx) > std::fabs(ry) ? rx : ry) : (std::fabs(hx - ox) > 0.5f ? rx : ry);
                sx = st.sx * k;
                sy = st.sy * k;
            }
            if (std::fabs(sx) < 0.01f) sx = sx < 0 ? -0.01f : 0.01f;
            if (std::fabs(sy) < 0.01f) sy = sy < 0 ? -0.01f : 0.01f;
            // t' = t + R(S (o - p)) - R(S' (o - p))
            const float ax = st.sx * (ox - st.px), ay = st.sy * (oy - st.py);
            const float bx = sx * (ox - st.px), by = sy * (oy - st.py);
            x.sx = sx;
            x.sy = sy;
            x.tx = st.tx + (c * ax - s * ay) - (c * bx - s * by);
            x.ty = st.ty + (s * ax + c * ay) - (s * bx + c * by);
        }
        transformApply(v);
    }
    if (m_crop.on && m_crop.drag >= 0 && down) {
        const ImVec2 q = snapped(p);
        const ImVec2 dd = sub(snapped(p), snapped(m_crop.press));
        float x0 = m_crop.sx0, y0 = m_crop.sy0, x1 = m_crop.sx1, y1 = m_crop.sy1;
        const int h = m_crop.drag;
        if (h == 8) { x0 += dd.x; x1 += dd.x; y0 += dd.y; y1 += dd.y; }
        else {
            if (kHandle[h][0] == 0.0f) x0 = q.x;
            if (kHandle[h][0] == 1.0f) x1 = q.x;
            if (kHandle[h][1] == 0.0f) y0 = q.y;
            if (kHandle[h][1] == 1.0f) y1 = q.y;
        }
        m_crop.x0 = x0; m_crop.y0 = y0; m_crop.x1 = x1; m_crop.y1 = y1;
    }
    if (m_anchorOn && m_anchorEdit) {
        const bool box = m_tool == Tool::Shape && m_shape != 0;
        const ImVec2 q = constrained(m_anchor, snapped(p), box);
        if (q.x == m_lastQ.x && q.y == m_lastQ.y) return;
        m_lastQ = q;
        if (m_tool == Tool::Shape)
            m_stroke.shape(img::Stroke::Shape(std::clamp(m_shape, 0, 4)), m_anchor.x, m_anchor.y, q.x, q.y, m_lineWidth);
        else if (m_tool == Tool::Gradient && d.editing())
            img::gradient(d, m_anchor.x, m_anchor.y, q.x, q.y, m_reverse ? m_bg : m_fg, m_reverse ? m_fg : m_bg, m_radial, m_fillOpacity);
    }
}

void ImageEditor::pointerUp(View& v, ImVec2 p, ImVec2 screen) {
    img::Document& d = v.doc;
    if (m_painting) {
        m_painting = false;
        m_stroke.end();
        v.lastPaint = m_lazy;
        v.hasLastPaint = true;
        if (m_tool == Tool::Brush) rememberColor(m_fg);
    }
    if (m_moving) {
        m_moving = false;
        if (m_moveDx == 0 && m_moveDy == 0) d.cancelEdit(); else d.endEdit();
    }
    if (m_selPending) {
        const bool moved = m_selMoving;
        m_selPending = m_selMoving = false;
        m_selBase.reset();
        if (!moved) {
            // It was a click: the first corner of a new selection.
            startAnchor(v, m_selPress, m_selPressScreen);
            m_anchorHeld = false;
        }
    }
    if (m_lasso.on && m_lasso.dragging) {
        m_lasso.dragging = false;
        lassoClose(v);
    }
    if (m_xf.on) m_xf.drag = -1;
    if (m_crop.on) m_crop.drag = -1;
    if (m_anchorOn && m_anchorHeld) {
        m_anchorHeld = false;
        // Let go far from where it was pressed: that was a drag, and it ends
        // here. Let go where it was pressed: a click, the second click ends it.
        const float th = std::max(6.0f, ImGui::GetIO().MouseDragThreshold);
        if (len(sub(screen, m_pressScreen)) > th) finishAnchor(v, p);
    }
}

void ImageEditor::doubleClick(View& v, ImVec2) {
    if (m_tool == Tool::Lasso && m_polyLasso && m_lasso.on) lassoClose(v);
    if (m_tool == Tool::Crop && m_crop.on) cropCommit(v);
}

void ImageEditor::finishAnchor(View& v, ImVec2 p) {
    img::Document& d = v.doc;
    const bool box = m_tool == Tool::Select || m_tool == Tool::Crop || (m_tool == Tool::Shape && m_shape != 0);
    const ImVec2 q = constrained(m_anchor, snapped(p), box && m_tool != Tool::Crop);
    m_anchorOn = false;
    m_anchorHeld = false;
    m_anchorEdit = false;
    if (m_tool == Tool::Select) {
        const img::Rect r{int(std::floor(std::min(m_anchor.x, q.x))), int(std::floor(std::min(m_anchor.y, q.y))),
                          int(std::ceil(std::max(m_anchor.x, q.x))), int(std::ceil(std::max(m_anchor.y, q.y)))};
        if (r.w() < 1 || r.h() < 1) { if (m_anchorOp == img::SelOp::Replace) d.deselect(); }
        else d.selectRect(r, m_anchorOp, m_ellipse);
    } else if (m_tool == Tool::Crop) {
        if (std::fabs(q.x - m_anchor.x) >= 1.0f && std::fabs(q.y - m_anchor.y) >= 1.0f) {
            m_crop = {};
            m_crop.on = true;
            m_crop.viewId = v.id;
            m_crop.x0 = std::min(m_anchor.x, q.x); m_crop.y0 = std::min(m_anchor.y, q.y);
            m_crop.x1 = std::max(m_anchor.x, q.x); m_crop.y1 = std::max(m_anchor.y, q.y);
        }
    } else if (m_tool == Tool::Shape) {
        m_stroke.shape(img::Stroke::Shape(std::clamp(m_shape, 0, 4)), m_anchor.x, m_anchor.y, q.x, q.y, m_lineWidth);
        m_stroke.end();
        rememberColor(m_fg);
    } else if (m_tool == Tool::Gradient) {
        if (q.x == m_anchor.x && q.y == m_anchor.y) { d.cancelEdit(); return; }
        img::gradient(d, m_anchor.x, m_anchor.y, q.x, q.y, m_reverse ? m_bg : m_fg, m_reverse ? m_fg : m_bg, m_radial, m_fillOpacity);
        d.endEdit();
    }
}

void ImageEditor::cancelGesture(View& v) {
    img::Document& d = v.doc;
    if (m_painting) { m_painting = false; m_stroke.end(); }
    if (m_moving) { m_moving = false; d.cancelEdit(); }
    if (m_anchorOn) {
        if (m_tool == Tool::Shape) m_stroke.cancel();
        else if (m_tool == Tool::Gradient) d.cancelEdit();
        m_anchorOn = m_anchorHeld = m_anchorEdit = false;
    }
    if (m_selPending) {
        m_selPending = m_selMoving = false;
        m_selBase.reset();
    }
    if (m_lasso.on) lassoCancel();
    m_lineHas = false;
    if (m_tool == Tool::Clone) m_clonePick = false;
    // A text being typed is not a gesture: it ends by Done, Cancel, Esc or the
    // next tool, and undo or anything else done to the layers ends it by itself.
    // A transform and a crop box are not either: they end by Enter or Esc.
}

// --- The canvas -----------------------------------------------------------------------

void ImageEditor::canvas(View& v) {
    img::Document& d = v.doc;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 60.0f), std::max(avail.y, 60.0f)};
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    m_center = add(c0, mul(size, 0.5f));
    if (v.fitPending) { fit(v, size); v.fitPending = false; }

    // Something else ended the transform's edit (a new layer, a filter ...): it was kept.
    if (m_xf.on && m_xf.viewId == v.id && !d.editing()) m_xf.on = false;
    textUpdate(v);
    upload(v);
    if (v.selVer != d.selectionVersion()) traceOutline(v);

    // Zoom about the pointer with the wheel; pan with the middle button, the
    // Hand tool, or Space held.
    if (hovered && io.MouseWheel != 0.0f)
        zoomAt(v, v.zoom * std::pow(1.2f, io.MouseWheel), io.MousePos);
    const bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !m_text.on;
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                    ((m_tool == Tool::Hand || space) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
        m_panning = true;
    if (m_panning) {
        v.pan = add(v.pan, io.MouseDelta);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            m_panning = false;
    }

    const float W = float(d.width()), H = float(d.height());
    auto originNow = [&] { return sub(add(m_center, v.pan), ImVec2(W * v.zoom * 0.5f, H * v.zoom * 0.5f)); };
    ImVec2 origin = originNow();
    ImVec2 mi = mul(sub(io.MousePos, origin), 1.0f / v.zoom);

    if (!m_panning && m_pane == Pane::None) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            pointerDown(v, mi, io.MousePos, false);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) doubleClick(v, mi);
        } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            pointerDown(v, mi, io.MousePos, true);
        }
        pointerDrag(v, mi, io.MousePos);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) pointerUp(v, mi, io.MousePos);
        upload(v);   // this frame's paint shows this frame
    }
    if (hovered) {
        if (m_tool == Tool::Hand || space || m_panning) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (m_xf.on) {
            const int h = m_xf.drag >= 0 ? m_xf.drag : transformHit(v, io.MousePos);
            ImGui::SetMouseCursor(h == 8 ? ImGuiMouseCursor_ResizeAll : h == 9 ? ImGuiMouseCursor_Arrow
                                  : h % 4 == 0 ? ImGuiMouseCursor_ResizeNWSE : h % 4 == 2 ? ImGuiMouseCursor_ResizeNESW
                                  : h % 4 == 1 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
        } else if (m_tool == Tool::Move) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        else if (m_tool == Tool::Text) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    // --- draw
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c1 = add(c0, size);
    dl->PushClipRect(c0, c1, true);
    dl->AddRectFilled(c0, c1, IM_COL32(38, 40, 44, 255));
    textureSampling(dl, true);
    origin = originNow();
    const ImVec2 o1 = add(origin, ImVec2(W * v.zoom, H * v.zoom));
    // The tile preview: the picture again all round it, a little dimmer.
    if (m_tilePreview) {
        const ImVec2 sz(W * v.zoom, H * v.zoom);
        for (int ty = -1; ty <= 1; ++ty)
            for (int tx = -1; tx <= 1; ++tx) {
                if (!tx && !ty) continue;
                const ImVec2 a = add(origin, ImVec2(sz.x * tx, sz.y * ty)), b = add(a, sz);
                if (b.x < c0.x || b.y < c0.y || a.x > c1.x || a.y > c1.y) continue;
                dl->AddImage((ImTextureID)(intptr_t)m_checker, a, b, ImVec2(0, 0), ImVec2(sz.x / 16.0f, sz.y / 16.0f),
                             IM_COL32(255, 255, 255, 190));
                dl->AddImage((ImTextureID)(intptr_t)v.tex, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 190));
            }
    }
    // Transparency shows as the usual grey checks, 8 screen pixels each.
    dl->AddImage((ImTextureID)(intptr_t)m_checker, origin, o1, ImVec2(0, 0),
                 ImVec2((o1.x - origin.x) / 16.0f, (o1.y - origin.y) / 16.0f));
    dl->AddImage((ImTextureID)(intptr_t)v.tex, origin, o1);
    textureSampling(dl, false);
    dl->AddRect(sub(origin, ImVec2(1, 1)), add(o1, ImVec2(1, 1)), IM_COL32(0, 0, 0, 160));

    // Pixel grid once pixels are big, the snap grid when it is coarse enough to see.
    auto grid = [&](float step, ImU32 col) {
        const float s = step * v.zoom;
        const float x0 = std::max(origin.x, c0.x), x1 = std::min(o1.x, c1.x);
        const float y0 = std::max(origin.y, c0.y), y1 = std::min(o1.y, c1.y);
        if (x1 <= x0 || y1 <= y0) return;
        for (float x = origin.x + std::ceil((x0 - origin.x) / s) * s; x <= x1; x += s) dl->AddLine({x, y0}, {x, y1}, col);
        for (float y = origin.y + std::ceil((y0 - origin.y) / s) * s; y <= y1; y += s) dl->AddLine({x0, y}, {x1, y}, col);
    };
    if (m_pixelGrid && v.zoom >= 8.0f) grid(1.0f, IM_COL32(128, 128, 128, 50));
    const bool snapTool = m_tool == Tool::Select || m_tool == Tool::Shape || m_tool == Tool::Move || m_tool == Tool::Crop ||
                          m_tool == Tool::Text || m_tool == Tool::Gradient;
    if (snapTool && m_snap > 1 && m_snap * v.zoom >= 8.0f) grid(float(m_snap), IM_COL32(90, 170, 255, 70));

    drawOverlay(v, dl, origin, mi, hovered);
    dl->PopClipRect();

    // The status line's pointer part.
    m_hoverInfo.clear();
    const int ix = int(std::floor(mi.x)), iy = int(std::floor(mi.y));
    if (hovered && ix >= 0 && iy >= 0 && ix < d.width() && iy < d.height()) {
        const img::Color c = d.sample(ix, iy, true);
        char buf[128];
        std::snprintf(buf, sizeof buf, "x %d  y %d   %s  alpha %d", ix, iy, img::hexOf(c).c_str(),
                      int(std::lround(c.a * 255.0f)));
        m_hoverInfo = buf;
    }
}

void ImageEditor::drawOverlay(View& v, ImDrawList* dl, ImVec2 origin, ImVec2 mi, bool hovered) {
    const img::Document& d = v.doc;
    const float z = v.zoom;
    auto S = [&](ImVec2 p) { return add(origin, mul(p, z)); };
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // The selection's edge: marching ants while the outline is small enough to dash.
    if (d.hasSelection() && m_showEdges && !(m_xf.on && m_xf.viewId == v.id)) {
        if (v.outlineBox) {
            const img::Rect b = d.selectionBounds();
            rect2(dl, S(ImVec2(float(b.x0), float(b.y0))), S(ImVec2(float(b.x1), float(b.y1))));
        } else {
            const ImVec2 cmin = dl->GetClipRectMin(), cmax = dl->GetClipRectMax();
            const bool ants = v.outline.size() < 30000;
            const float phase = float(std::fmod(ImGui::GetTime() * 12.0, 8.0));
            for (const ImVec4& s : v.outline) {
                const ImVec2 a = S(ImVec2(s.x, s.y)), b = S(ImVec2(s.z, s.w));
                if (std::max(a.x, b.x) < cmin.x || std::min(a.x, b.x) > cmax.x ||
                    std::max(a.y, b.y) < cmin.y || std::min(a.y, b.y) > cmax.y) continue;
                dl->AddLine(a, b, ants ? IM_COL32(255, 255, 255, 235) : kInk, ants ? 1.0f : 3.0f);
                if (!ants) { dl->AddLine(a, b, kPaper, 1.0f); continue; }
                // Black dashes, 4 px on and 4 off, crawling along the edge.
                const bool horiz = s.y == s.w;
                const float from = horiz ? a.x : a.y, to = horiz ? b.x : b.y;
                const float base = horiz ? a.y : a.x;
                float m = std::fmod(from + base - phase, 8.0f);
                if (m < 0.0f) m += 8.0f;
                for (float t = from - m; t < to; t += 8.0f) {
                    const float t0 = std::max(t, from), t1 = std::min(t + 4.0f, to);
                    if (t1 <= t0) continue;
                    if (horiz) dl->AddLine(ImVec2(t0, base), ImVec2(t1, base), IM_COL32(0, 0, 0, 255), 1.0f);
                    else dl->AddLine(ImVec2(base, t0), ImVec2(base, t1), IM_COL32(0, 0, 0, 255), 1.0f);
                }
            }
        }
    }

    // What the gesture in progress will make.
    if (m_anchorOn) {
        const bool box = m_tool == Tool::Select || (m_tool == Tool::Shape && m_shape != 0);
        const ImVec2 q = constrained(m_anchor, snapped(mi), box);
        const ImVec2 a = S(m_anchor), b = S(q);
        if (m_tool == Tool::Select) {
            if (m_ellipse) ellipse2(dl, a, b); else rect2(dl, a, b);
        } else if (m_tool == Tool::Crop) {
            rect2(dl, a, S(snapped(mi)));
        } else if (m_tool == Tool::Gradient) {
            line2(dl, a, b);
            circle2(dl, a, 5.0f);
            circle2(dl, b, 5.0f);
        }
        circle2(dl, a, 3.0f);
    }
    // The lasso so far, and the side the next click would add.
    if (m_lasso.on && m_lasso.viewId == v.id && !m_lasso.pts.empty()) {
        std::vector<ImVec2> pts;
        for (const ImVec2& p : m_lasso.pts) pts.push_back(S(p));
        if (m_lasso.dragging) pts.push_back(S(mi));
        dl->AddPolyline(pts.data(), int(pts.size()), kInk, 0, 3.0f);
        dl->AddPolyline(pts.data(), int(pts.size()), kPaper, 0, 1.0f);
        if (!m_lasso.dragging && hovered) {
            line2(dl, pts.back(), mouse);
            const bool nearStart = m_lasso.pts.size() >= 3 && len(sub(pts.front(), mouse)) <= 9.0f;
            circle2(dl, pts.front(), nearStart ? 7.0f : 4.0f);
            for (const ImVec2& p : pts) handle2(dl, p, 2.5f);
        }
    }
    // The crop box: what goes darkens, the rule of thirds inside, handles round it.
    if (m_crop.on && m_crop.viewId == v.id) {
        const ImVec2 a = S(ImVec2(std::min(m_crop.x0, m_crop.x1), std::min(m_crop.y0, m_crop.y1)));
        const ImVec2 b = S(ImVec2(std::max(m_crop.x0, m_crop.x1), std::max(m_crop.y0, m_crop.y1)));
        const ImVec2 cmin = dl->GetClipRectMin(), cmax = dl->GetClipRectMax();
        const ImU32 shade = IM_COL32(0, 0, 0, 140);
        dl->AddRectFilled(cmin, ImVec2(cmax.x, a.y), shade);
        dl->AddRectFilled(ImVec2(cmin.x, b.y), cmax, shade);
        dl->AddRectFilled(ImVec2(cmin.x, a.y), ImVec2(a.x, b.y), shade);
        dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(cmax.x, b.y), shade);
        for (int k = 1; k < 3; ++k) {
            const float x = a.x + (b.x - a.x) * k / 3.0f, y = a.y + (b.y - a.y) * k / 3.0f;
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 255, 255, 90));
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 90));
        }
        rect2(dl, a, b);
        for (const auto& h : kHandle) handle2(dl, ImVec2(a.x + (b.x - a.x) * h[0], a.y + (b.y - a.y) * h[1]));
    }
    // Free transform: the box as it now lies, its handles, the pivot.
    if (m_xf.on && m_xf.viewId == v.id) {
        ImVec2 q[4];
        transformCorners(q);
        for (ImVec2& p : q) p = S(p);
        dl->AddPolyline(q, 4, kInk, ImDrawFlags_Closed, 3.0f);
        dl->AddPolyline(q, 4, kPaper, ImDrawFlags_Closed, 1.0f);
        const img::Rect& r = m_xf.src;
        for (const auto& h : kHandle) {
            ImVec2 p;
            m_xf.xf.apply(r.x0 + r.w() * h[0], r.y0 + r.h() * h[1], p.x, p.y);
            handle2(dl, S(p));
        }
        const ImVec2 c = S(ImVec2(m_xf.xf.px + m_xf.xf.tx, m_xf.xf.py + m_xf.xf.ty));
        circle2(dl, c, 4.0f);
        line2(dl, ImVec2(c.x - 7, c.y), ImVec2(c.x + 7, c.y));
        line2(dl, ImVec2(c.x, c.y - 7), ImVec2(c.x, c.y + 7));
        // Outside the box the pointer turns it: say so beside the pointer.
        if (hovered && (m_xf.drag == 9 || (m_xf.drag < 0 && transformHit(v, mouse) == 9)))
            drawIcon(dl, Icon::RotateCW, ImVec2(mouse.x + 16, mouse.y + 14), 16.0f, kPaper);
    }
    const bool brushTool = m_tool == Tool::Brush || m_tool == Tool::Eraser || m_tool == Tool::Clone ||
                           m_tool == Tool::Blur || m_tool == Tool::Dodge;
    if (brushTool && hovered && !m_xf.on) {
        if (m_lineMode && m_lineHas) line2(dl, S(m_lineLast), S(constrained(m_lineLast, mi, false)));
        else if (ImGui::GetIO().KeyShift && v.hasLastPaint && !m_painting) line2(dl, S(v.lastPaint), S(constrained(v.lastPaint, mi, false)));
    }
    if (m_tool == Tool::Clone && m_cloneHasSrc) {
        const ImVec2 src = m_cloneAligned ? S(add(mi, m_cloneOff)) : S(m_cloneSrc);
        line2(dl, ImVec2(src.x - 8, src.y), ImVec2(src.x + 8, src.y));
        line2(dl, ImVec2(src.x, src.y - 8), ImVec2(src.x, src.y + 8));
    }
    const bool picking = m_tool == Tool::Clone && (m_clonePick || !m_cloneHasSrc || ImGui::GetIO().KeyAlt);
    if (brushTool && hovered && !picking && !m_xf.on) {
        const float r = std::max(2.0f, m_brush.radius * z);
        if (m_painting) {
            const ImVec2 l = S(m_lazy);
            line2(dl, l, mouse);
            circle2(dl, l, r);
        } else {
            circle2(dl, mouse, r);
        }
    }
    if (picking && hovered) {
        line2(dl, ImVec2(mouse.x - 10, mouse.y), ImVec2(mouse.x + 10, mouse.y));
        line2(dl, ImVec2(mouse.x, mouse.y - 10), ImVec2(mouse.x, mouse.y + 10));
        circle2(dl, mouse, 6.0f);
    }
    // The zoom tool and the eyedropper carry their picture beside the pointer.
    if (hovered && !m_xf.on && (m_tool == Tool::Zoom || m_tool == Tool::Eyedropper)) {
        const Icon ic = m_tool == Tool::Eyedropper ? Icon::Eyedropper : ImGui::GetIO().KeyAlt ? Icon::ZoomOut : Icon::ZoomIn;
        drawIcon(dl, ic, ImVec2(mouse.x + 15, mouse.y + 15), 18.0f, kInk);
        drawIcon(dl, ic, ImVec2(mouse.x + 14, mouse.y + 14), 18.0f, kPaper);
    }
    if (m_text.on && m_text.viewId == v.id) {
        const ImVec2 p = S(m_text.pos);
        line2(dl, p, ImVec2(p.x, p.y + m_textSize * z));
        circle2(dl, p, 3.0f);
    }
}

} // namespace imageui
