// The Image editor's canvas: the picture on screen (a GL texture kept in step
// with the document's dirty rectangles), pan and zoom, and what each tool does
// with the pointer. The window around it is ImageEditPanel.cpp.
#include "ImageEditPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glad/gl.h>
#include <imgui.h>

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

const float kZooms[] = {1.0f / 32, 1.0f / 16, 1.0f / 8, 1.0f / 6, 0.25f, 1.0f / 3, 0.5f, 2.0f / 3, 1.0f,
                        1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f};

} // namespace

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

// --- Gestures -------------------------------------------------------------------------

void ImageEditor::pointerDown(View& v, ImVec2 p, ImVec2 screen, bool right) {
    img::Document& d = v.doc;
    const int ix = int(std::floor(p.x)), iy = int(std::floor(p.y));
    if (right) {
        // A right click picks the colour under it -- the eyedropper without
        // changing tools (the Eyedropper tool itself: into the background colour).
        if (ix < 0 || iy < 0 || ix >= d.width() || iy >= d.height()) return;
        const img::Color c = d.sample(ix, iy, true);
        if (m_tool == Tool::Eyedropper) m_bg = c; else m_fg = c;
        return;
    }
    switch (m_tool) {
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Clone: {
        if (m_tool == Tool::Clone && (m_clonePick || !m_cloneHasSrc)) {
            m_cloneSrc = p;
            m_cloneHasSrc = true;
            m_clonePick = false;
            m_cloneAligned = false;
            m_d.status = "Clone source set -- now paint where the copy should go.";
            return;
        }
        const img::PaintMode mode = m_tool == Tool::Eraser ? img::PaintMode::Erase
                                  : m_tool == Tool::Clone  ? img::PaintMode::Clone : img::PaintMode::Paint;
        if (m_tool == Tool::Clone && !m_cloneAligned) {
            m_cloneOff = sub(m_cloneSrc, p);
            m_cloneAligned = true;
        }
        const ImVec2 off = m_tool == Tool::Clone ? m_cloneOff : ImVec2(0, 0);
        if (m_lineMode) {
            const ImVec2 q = m_lineHas ? constrained(m_lineLast, p, false) : p;
            m_stroke.begin(d, mode, m_fg, m_brush, off.x, off.y);
            if (m_lineHas) m_stroke.to(m_lineLast.x, m_lineLast.y);
            m_stroke.to(q.x, q.y);
            m_stroke.end();
            m_lineLast = q;
            m_lineHas = true;
            if (mode == img::PaintMode::Paint) rememberColor(m_fg);
            return;
        }
        m_stroke.begin(d, mode, m_fg, m_brush, off.x, off.y);
        m_stroke.to(p.x, p.y);
        m_lazy = p;
        m_painting = true;
        return;
    }
    case Tool::Select:
    case Tool::Shape:
    case Tool::Gradient:
        if (!m_anchorOn) {
            m_anchorOn = true;
            m_anchorHeld = true;
            m_anchor = snapped(p);
            m_pressScreen = screen;
            m_lastQ = {-1e9f, -1e9f};
            m_anchorEdit = false;
            if (m_tool == Tool::Shape) {
                static const char* names[] = {"Line", "Rectangle", "Rectangle", "Ellipse", "Ellipse"};
                img::Brush b = m_brush;
                m_stroke.begin(d, img::PaintMode::Paint, m_fg, b, 0, 0, names[std::clamp(m_shape, 0, 4)]);
                m_anchorEdit = true;
            } else if (m_tool == Tool::Gradient) {
                d.beginEdit("Gradient");
                m_anchorEdit = true;
            }
        } else {
            finishAnchor(v, p);
        }
        return;
    case Tool::Wand:
        d.selectColor(ix, iy, m_tolerance, m_contiguous, m_sampleMerged, m_selOp);
        return;
    case Tool::Fill:
        if (ix < 0 || iy < 0 || ix >= d.width() || iy >= d.height()) return;
        d.floodFill(ix, iy, m_fg, m_tolerance, m_contiguous, m_sampleMerged, m_fillOpacity);
        rememberColor(m_fg);
        return;
    case Tool::Eyedropper:
        if (ix < 0 || iy < 0 || ix >= d.width() || iy >= d.height()) return;
        m_fg = d.sample(ix, iy, m_sampleMerged);
        return;
    case Tool::Move:
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

void ImageEditor::pointerDrag(View& v, ImVec2 p) {
    img::Document& d = v.doc;
    if (m_painting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
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
    if (m_moving && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float s = float(std::max(1, m_snap));
        int dx = int(std::round((p.x - m_moveStart.x) / s) * s);
        int dy = int(std::round((p.y - m_moveStart.y) / s) * s);
        if (m_constrain || ImGui::GetIO().KeyShift) { if (std::abs(dx) > std::abs(dy)) dy = 0; else dx = 0; }
        if (dx != m_moveDx || dy != m_moveDy) {
            m_moveDx = dx; m_moveDy = dy;
            d.moveSelected(dx, dy);
        }
    }
    if (m_anchorOn && m_anchorEdit) {
        const bool box = m_tool == Tool::Shape && m_shape != 0;
        const ImVec2 q = constrained(m_anchor, snapped(p), box);
        if (q.x == m_lastQ.x && q.y == m_lastQ.y) return;
        m_lastQ = q;
        if (m_tool == Tool::Shape)
            m_stroke.shape(img::Stroke::Shape(std::clamp(m_shape, 0, 4)), m_anchor.x, m_anchor.y, q.x, q.y, m_lineWidth);
        else if (m_tool == Tool::Gradient && d.editing())
            img::gradient(d, m_anchor.x, m_anchor.y, q.x, q.y, m_fg, m_bg, m_radial, m_fillOpacity);
    }
}

void ImageEditor::pointerUp(View& v, ImVec2 p, ImVec2 screen) {
    img::Document& d = v.doc;
    if (m_painting) {
        m_painting = false;
        m_stroke.end();
        if (m_tool == Tool::Brush) rememberColor(m_fg);
    }
    if (m_moving) {
        m_moving = false;
        if (m_moveDx == 0 && m_moveDy == 0) d.cancelEdit(); else d.endEdit();
    }
    if (m_anchorOn && m_anchorHeld) {
        m_anchorHeld = false;
        // Let go far from where it was pressed: that was a drag, and it ends
        // here. Let go where it was pressed: a click, the second click ends it.
        const float th = std::max(6.0f, ImGui::GetIO().MouseDragThreshold);
        if (len(sub(screen, m_pressScreen)) > th) finishAnchor(v, p);
    }
}

void ImageEditor::finishAnchor(View& v, ImVec2 p) {
    img::Document& d = v.doc;
    const bool box = m_tool == Tool::Select || (m_tool == Tool::Shape && m_shape != 0);
    const ImVec2 q = constrained(m_anchor, snapped(p), box);
    m_anchorOn = false;
    m_anchorHeld = false;
    m_anchorEdit = false;
    if (m_tool == Tool::Select) {
        const img::Rect r{int(std::floor(std::min(m_anchor.x, q.x))), int(std::floor(std::min(m_anchor.y, q.y))),
                          int(std::ceil(std::max(m_anchor.x, q.x))), int(std::ceil(std::max(m_anchor.y, q.y)))};
        if (r.w() < 1 || r.h() < 1) { if (m_selOp == img::SelOp::Replace) d.deselect(); }
        else d.selectRect(r, m_selOp, m_ellipse);
    } else if (m_tool == Tool::Shape) {
        m_stroke.shape(img::Stroke::Shape(std::clamp(m_shape, 0, 4)), m_anchor.x, m_anchor.y, q.x, q.y, m_lineWidth);
        m_stroke.end();
        rememberColor(m_fg);
    } else if (m_tool == Tool::Gradient) {
        if (q.x == m_anchor.x && q.y == m_anchor.y) { d.cancelEdit(); return; }
        img::gradient(d, m_anchor.x, m_anchor.y, q.x, q.y, m_fg, m_bg, m_radial, m_fillOpacity);
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
    m_lineHas = false;
    if (m_tool == Tool::Clone) m_clonePick = false;
    // A text being typed is not a gesture: it ends by Done, Cancel, Esc or the
    // next tool, and undo or anything else done to the layers ends it by itself.
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
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pointerDown(v, mi, io.MousePos, false);
        else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) pointerDown(v, mi, io.MousePos, true);
        pointerDrag(v, mi);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) pointerUp(v, mi, io.MousePos);
        upload(v);   // this frame's paint shows this frame
    }
    if (hovered) {
        if (m_tool == Tool::Hand || space || m_panning) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (m_tool == Tool::Move) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        else if (m_tool == Tool::Text) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    // --- draw
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c1 = add(c0, size);
    dl->PushClipRect(c0, c1, true);
    dl->AddRectFilled(c0, c1, IM_COL32(38, 40, 44, 255));
    origin = originNow();
    const ImVec2 o1 = add(origin, ImVec2(W * v.zoom, H * v.zoom));
    // Transparency shows as the usual grey checks, 8 screen pixels each.
    dl->AddImage((ImTextureID)(intptr_t)m_checker, origin, o1, ImVec2(0, 0),
                 ImVec2((o1.x - origin.x) / 16.0f, (o1.y - origin.y) / 16.0f));
    dl->AddImage((ImTextureID)(intptr_t)v.tex, origin, o1);
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
    if (v.zoom >= 8.0f) grid(1.0f, IM_COL32(128, 128, 128, 50));
    const bool snapTool = m_tool == Tool::Select || m_tool == Tool::Shape || m_tool == Tool::Move ||
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

    // The selection's edge.
    if (d.hasSelection()) {
        if (v.outlineBox) {
            const img::Rect b = d.selectionBounds();
            rect2(dl, S(ImVec2(float(b.x0), float(b.y0))), S(ImVec2(float(b.x1), float(b.y1))));
        } else {
            const ImVec4 clip = dl->GetClipRectMax().x > 0 ? ImVec4(dl->GetClipRectMin().x, dl->GetClipRectMin().y,
                                                                     dl->GetClipRectMax().x, dl->GetClipRectMax().y)
                                                           : ImVec4(0, 0, 0, 0);
            for (int pass = 0; pass < 2; ++pass)
                for (const ImVec4& s : v.outline) {
                    const ImVec2 a = S(ImVec2(s.x, s.y)), b = S(ImVec2(s.z, s.w));
                    if (std::max(a.x, b.x) < clip.x || std::min(a.x, b.x) > clip.z ||
                        std::max(a.y, b.y) < clip.y || std::min(a.y, b.y) > clip.w) continue;
                    if (pass == 0) dl->AddLine(a, b, kInk, 3.0f);
                    else dl->AddLine(a, b, kPaper, 1.0f);
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
        } else if (m_tool == Tool::Gradient) {
            line2(dl, a, b);
            circle2(dl, a, 5.0f);
            circle2(dl, b, 5.0f);
        }
        circle2(dl, a, 3.0f);
    }
    const bool brushTool = m_tool == Tool::Brush || m_tool == Tool::Eraser || m_tool == Tool::Clone;
    if (brushTool && m_lineMode && m_lineHas && hovered) {
        line2(dl, S(m_lineLast), S(constrained(m_lineLast, mi, false)));
    }
    if (m_tool == Tool::Clone && m_cloneHasSrc) {
        const ImVec2 src = m_cloneAligned ? S(add(mi, m_cloneOff)) : S(m_cloneSrc);
        line2(dl, ImVec2(src.x - 8, src.y), ImVec2(src.x + 8, src.y));
        line2(dl, ImVec2(src.x, src.y - 8), ImVec2(src.x, src.y + 8));
    }
    if (brushTool && hovered && !(m_tool == Tool::Clone && (m_clonePick || !m_cloneHasSrc))) {
        const float r = std::max(2.0f, m_brush.radius * z);
        if (m_painting) {
            const ImVec2 l = S(m_lazy);
            line2(dl, l, mouse);
            circle2(dl, l, r);
        } else {
            circle2(dl, mouse, r);
        }
    }
    if ((m_tool == Tool::Clone && (m_clonePick || !m_cloneHasSrc)) && hovered) {
        line2(dl, ImVec2(mouse.x - 10, mouse.y), ImVec2(mouse.x + 10, mouse.y));
        line2(dl, ImVec2(mouse.x, mouse.y - 10), ImVec2(mouse.x, mouse.y + 10));
        circle2(dl, mouse, 6.0f);
    }
    if (m_text.on && m_text.viewId == v.id) {
        const ImVec2 p = S(m_text.pos);
        line2(dl, p, ImVec2(p.x, p.y + m_textSize * z));
        circle2(dl, p, 3.0f);
    }
}

} // namespace imageui
