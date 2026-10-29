#include "ImageEditPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>

#include <imgui.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "FolderDialog.hpp"
#include "ImageIcons.hpp"
#include "UiStyle.hpp"
#include "fitzel/rhi/Device.hpp"
#include "fitzel/ui/ImGuiTexture.hpp"

namespace fs = std::filesystem;

namespace imageui {

float nextZoom(float z, int dir);                 // ImageCanvas.cpp
void  textureSampling(ImDrawList* dl, bool own);  // ImageCanvas.cpp

namespace {

struct ToolDef { const char* name; const char* key; ImGuiKey imkey; const char* tip; };
const ToolDef kTools[] = {
    {"Move", "V", ImGuiKey_V, "Moves what is selected -- the whole layer with nothing selected. Drag, or the arrow keys.\n"
                              "Ctrl+T: free transform (scale, turn, flip)."},
    {"Marquee", "M", ImGuiKey_M, "Selects a rectangle or an ellipse (Shift+M switches). Click one corner, click the other -- or drag.\n"
                                 "Shift: add to the selection, Alt: take away. Drag inside the selection to move its outline."},
    {"Lasso", "L", ImGuiKey_L, "Selects a shape you draw round. Shift+L: polygonal -- click corner after corner,\n"
                               "click the first point again (or double-click, or Enter) to close it. No dragging needed."},
    {"Magic wand", "W", ImGuiKey_W, "Selects by colour: one click takes everything near the colour under it."},
    {"Crop", "C", ImGuiKey_C, "Click two corners (or drag), adjust the handles or the numbers, Enter crops."},
    {"Eyedropper", "I", ImGuiKey_I, "Click: foreground colour. Right click: background colour.\n"
                                    "With the painting tools, Alt+click picks without changing tools."},
    {"Brush", "B", ImGuiKey_B, "Paints. Going over a spot again does not darken it.\n"
                               "Shift+click: a straight line from the last stroke. Right or Alt+click: pick a colour."},
    {"Clone stamp", "S", ImGuiKey_S, "Copies one part of the picture over another: Alt+click the source, then paint."},
    {"Eraser", "E", ImGuiKey_E, "Paints transparency."},
    {"Gradient", "G", ImGuiKey_G, "Foreground to background colour, from the first click to the second (or a drag).\n"
                                  "Shift+G: paint bucket."},
    {"Paint bucket", "G", ImGuiKey_K, "Fills the area of similar colour that was clicked. (K too.)"},
    {"Blur / Sharpen", "R", ImGuiKey_R, "Softens or crisps where you paint (Shift+R switches)."},
    {"Dodge / Burn / Sponge", "O", ImGuiKey_O, "Lightens, darkens, or takes colour out (or puts it in) where you paint.\n"
                                               "Shift+O switches."},
    {"Type", "T", ImGuiKey_T, "Click where the text goes and type; it becomes a layer of its own."},
    {"Shapes", "U", ImGuiKey_U, "Lines, rectangles and ellipses, outlined or filled. Click -- click, or drag. Shift+U switches."},
    {"Hand", "H", ImGuiKey_H, "Moves the view. Space and the middle mouse button do it with any tool."},
    {"Zoom", "Z", ImGuiKey_Z, "Click: zoom in there. Alt+click or right click: zoom out."},
};
static_assert(sizeof(kTools) / sizeof(kTools[0]) == std::size_t(Tool::Count));

const img::Color kPalette[] = {
    {0, 0, 0, 1}, {0.25f, 0.25f, 0.25f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.75f, 0.75f, 0.75f, 1}, {1, 1, 1, 1},
    {0.85f, 0.12f, 0.1f, 1}, {0.95f, 0.55f, 0.1f, 1}, {0.98f, 0.85f, 0.15f, 1}, {0.2f, 0.7f, 0.25f, 1}, {0.15f, 0.75f, 0.8f, 1},
    {0.15f, 0.35f, 0.85f, 1}, {0.5f, 0.25f, 0.75f, 1}, {0.85f, 0.25f, 0.6f, 1}, {0.9f, 0.72f, 0.6f, 1}, {0.6f, 0.25f, 0.2f, 1},
    {0.36f, 0.25f, 0.15f, 1}, {0.76f, 0.6f, 0.42f, 1}, {0.86f, 0.78f, 0.6f, 1}, {0.55f, 0.53f, 0.5f, 1}, {0.35f, 0.33f, 0.3f, 1},
    {0.42f, 0.45f, 0.2f, 1}, {0.3f, 0.4f, 0.2f, 1}, {0.18f, 0.28f, 0.14f, 1}, {0.53f, 0.73f, 0.9f, 1}, {0.15f, 0.35f, 0.45f, 1},
};

const char* const kFilterMenus[] = {"Blur", "Sharpen", "Noise", "Pixelate", "Render", "Stylize", "Texture", "Other"};

ImVec4 toIm(const img::Color& c) { return ImVec4(c.r, c.g, c.b, c.a); }

float em() { return ImGui::GetFontSize(); }

// A button tall enough to hit without aiming.
bool bigButton(const char* label, const char* tip = nullptr, float w = 0.0f) {
    const bool r = ImGui::Button(label, ImVec2(w, ImGui::GetFrameHeight() + em() * 0.45f));
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return r;
}

// A button that shows whether it is on.
bool toggle(const char* label, bool on, float w = 0.0f, const char* tip = nullptr) {
    if (on) {
        const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    }
    const bool r = bigButton(label, tip, w);
    if (on) ImGui::PopStyleColor(2);
    return r;
}

// Width that fits `n` buttons across what is left of the row.
float across(int n) {
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    return std::floor((ImGui::GetContentRegionAvail().x - sp * float(n - 1)) / float(n));
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool saveable(const std::string& path) {
    const std::string e = lower(fs::path(path).extension().string());
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".psd";
}

// The options bar lays its items out left to right and starts a new row when
// the next one would not fit.
struct Flow {
    float right;
    bool first = true;
    void next(float w) {
        if (!first) {
            ImGui::SameLine();
            if (ImGui::GetCursorPosX() + w > right) ImGui::NewLine();
        }
        first = false;
    }
    // A thin upright rule between two groups.
    void rule() {
        if (first) return;
        ImGui::SameLine(0.0f, em() * 0.5f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.15f), ImVec2(p.x, p.y + h * 0.85f),
                                            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        ImGui::Dummy(ImVec2(em() * 0.5f, h));
    }
};

// The options bar's row: as tall as a stepper, so every tool's bar is the same
// height and the canvas below does not jump when the tool changes.
float barRow() { return ImGui::GetFrameHeight() + em() * 0.35f; }

bool flowIcon(Flow& f, const char* id, Icon ic, const char* tip, bool on = false) {
    f.next(barRow());
    return iconButton(id, ic, tip, on, barRow());
}

bool flowStepper(Flow& f, const char* label, const char* id, float& v, float step, float lo, float hi,
                 const char* fmt, const char* tip = nullptr, float width = 0.0f) {
    const float w = width > 0.0f ? width : std::max(em() * 7.5f, ui::stepperWidth(fmt));
    f.next(ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x + w);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const bool r = ui::stepper(id, v, step, lo, hi, fmt, w);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return r;
}

// A 0..1 amount shown as a percentage.
bool flowPercent(Flow& f, const char* label, const char* id, float& v01, float step, const char* tip = nullptr) {
    float p = v01 * 100.0f;
    if (!flowStepper(f, label, id, p, step, 0.0f, 100.0f, "%.0f %%", tip)) return false;
    v01 = p / 100.0f;
    return true;
}

bool flowCheck(Flow& f, const char* label, bool* v, const char* tip = nullptr) {
    f.next(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x);
    const bool r = ImGui::Checkbox(label, v);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return r;
}

bool flowButton(Flow& f, const char* label, const char* tip = nullptr) {
    f.next(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    const bool r = ImGui::Button(label);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return r;
}

void flowText(Flow& f, const char* text) {
    f.next(ImGui::CalcTextSize(text).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
}

// --- The system clipboard (pictures) ----------------------------------------------------

#ifdef _WIN32
bool clipboardHasImage() { return IsClipboardFormatAvailable(CF_DIB) != 0; }
std::uint32_t clipboardSeq() { return GetClipboardSequenceNumber(); }

bool readClipboard(img::Clip& c) {
    if (!IsClipboardFormatAvailable(CF_DIB) || !OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_DIB)) {
        if (const auto* bi = static_cast<const BITMAPINFOHEADER*>(GlobalLock(h))) {
            const int w = bi->biWidth, hgt = std::abs(bi->biHeight), bpp = bi->biBitCount;
            const bool bottomUp = bi->biHeight > 0;
            if (w > 0 && hgt > 0 && (bpp == 24 || bpp == 32) &&
                (bi->biCompression == BI_RGB || bi->biCompression == BI_BITFIELDS)) {
                const std::uint8_t* bits = reinterpret_cast<const std::uint8_t*>(bi) + bi->biSize +
                                           (bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0) +
                                           bi->biClrUsed * 4;
                const std::size_t stride = ((std::size_t(w) * bpp + 31) / 32) * 4;
                c.w = w; c.h = hgt; c.x = 0; c.y = 0;
                c.px.assign(std::size_t(w) * hgt * 4, 255);
                bool anyAlpha = false;
                for (int y = 0; y < hgt; ++y) {
                    const std::uint8_t* row = bits + stride * std::size_t(bottomUp ? hgt - 1 - y : y);
                    for (int x = 0; x < w; ++x) {
                        const std::uint8_t* s = row + std::size_t(x) * (bpp / 8);
                        std::uint8_t* d = &c.px[(std::size_t(y) * w + x) * 4];
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
                        d[3] = bpp == 32 ? s[3] : 255;
                        anyAlpha |= bpp == 32 && s[3] != 0;
                    }
                }
                // Most programs leave the fourth byte at zero: that is "no alpha", not "invisible".
                if (bpp == 32 && !anyAlpha) for (std::size_t i = 3; i < c.px.size(); i += 4) c.px[i] = 255;
                ok = true;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}

void writeClipboard(const img::Clip& c) {
    if (!c.valid()) return;
    const std::size_t bytes = sizeof(BITMAPINFOHEADER) + std::size_t(c.w) * c.h * 4;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return;
    auto* bi = static_cast<BITMAPINFOHEADER*>(GlobalLock(h));
    std::memset(bi, 0, sizeof *bi);
    bi->biSize = sizeof *bi;
    bi->biWidth = c.w;
    bi->biHeight = c.h;   // bottom-up
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    auto* bits = reinterpret_cast<std::uint8_t*>(bi + 1);
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            const std::uint8_t* s = &c.px[(std::size_t(c.h - 1 - y) * c.w + x) * 4];
            std::uint8_t* d = bits + (std::size_t(y) * c.w + x) * 4;
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    GlobalUnlock(h);
    // The clipboard needs an owner window, or SetClipboardData refuses.
    if (!OpenClipboard(GetActiveWindow())) { GlobalFree(h); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, h)) GlobalFree(h);
    CloseClipboard();
}
#else
bool clipboardHasImage() { return false; }
std::uint32_t clipboardSeq() { return 0; }
bool readClipboard(img::Clip&) { return false; }
void writeClipboard(const img::Clip&) {}
#endif

// A transform that lands its result on whole pixels (flips and quarter turns
// should move pixels, not resample them).
void snapToPixels(img::Xform& x, const img::Rect& src) {
    float lx = 1e30f, ly = 1e30f;
    for (int k = 0; k < 4; ++k) {
        float ox, oy;
        x.apply(float(k & 1 ? src.x1 : src.x0), float(k & 2 ? src.y1 : src.y0), ox, oy);
        lx = std::min(lx, ox); ly = std::min(ly, oy);
    }
    x.tx += std::round(lx) - lx;
    x.ty += std::round(ly) - ly;
}

} // namespace

// --- Life -----------------------------------------------------------------------------

ImageEditor::ImageEditor(Deps d) : m_d(d) {
    for (auto& c : m_curve) c = {{0.0f, 0.0f}, {255.0f, 255.0f}};
}

ImageEditor::~ImageEditor() {
    for (auto& v : m_views) {
        if (v->tex) m_retired.push_back(v->tex);
        for (fitzel::rhi::TextureHandle t : v->thumbs) m_retired.push_back(t);
    }
    if (m_checker) m_retired.push_back(m_checker);
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    for (fitzel::rhi::TextureHandle t : m_retired) gpu.destroy(t);
}

bool ImageEditor::hasKeyboard() const { return m_focusFrame >= ImGui::GetFrameCount() - 1; }

img::Document* ImageEditor::document() { View* v = cur(); return v ? &v->doc : nullptr; }

void ImageEditor::setTool(Tool t) {
    if (t == m_tool) return;
    textEnd(true);
    if (View* v = cur()) {
        cancelGesture(*v);
        if (m_xf.on) transformCommit(*v);
    }
    lassoCancel();
    cropCancel();
    m_tool = t;
    if (t == Tool::Clone && !m_cloneHasSrc) m_clonePick = true;
}

std::string ImageEditor::projectDir() const {
    if (m_d.currentProject.empty()) return {};
    return fs::path(m_d.currentProject).parent_path().generic_string();
}

std::string ImageEditor::startDir() const {
    if (!m_lastDir.empty()) return m_lastDir;
    const std::string p = projectDir();
    if (!p.empty()) {
        std::error_code ec;
        for (const char* sub : {"textures", "Textures", "images"})
            if (fs::is_directory(fs::path(p) / sub, ec)) return (fs::path(p) / sub).generic_string();
    }
    return p;
}

void ImageEditor::scanImages() {
    m_scannedFor = projectDir();
    m_images.clear();
    if (m_scannedFor.empty()) return;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_scannedFor, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (!name.empty() && (name[0] == '.' || name == "build" || name == "export")) it.disable_recursion_pending();
            continue;
        }
        if (img::isImageFile(name)) m_images.push_back(it->path().generic_string());
        if (m_images.size() >= 5000) break;
    }
    std::sort(m_images.begin(), m_images.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
}

void ImageEditor::newImage(int w, int h, img::Color fill) {
    auto v = std::make_unique<View>();
    v->doc.create(w, h, fill);
    v->id = m_nextId++;
    m_views.push_back(std::move(v));
    m_cur = int(m_views.size()) - 1;
    m_selectTab = m_cur;
}

bool ImageEditor::open(const std::string& pathIn) {
    const std::string path = fs::path(pathIn).generic_string();
    for (std::size_t i = 0; i < m_views.size(); ++i)
        if (lower(m_views[i]->doc.path()) == lower(path)) { m_cur = int(i); m_selectTab = int(i); return true; }
    auto v = std::make_unique<View>();
    std::string err;
    if (!v->doc.load(path, err)) { m_d.status = err; return false; }
    v->id = m_nextId++;
    m_views.push_back(std::move(v));
    m_cur = int(m_views.size()) - 1;
    m_selectTab = m_cur;
    m_lastDir = fs::path(path).parent_path().generic_string();
    const img::Document& d = m_views.back()->doc;
    m_d.status = "Opened " + d.title() + " (" + std::to_string(d.width()) + " x " + std::to_string(d.height()) +
                 (d.layerCount() > 1 ? ", " + std::to_string(d.layerCount()) + " layers)" : ")");
    return true;
}

void ImageEditor::openDialog() {
    std::string out;
    if (ed::pickFile(out, startDir(), "Images", "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif")) open(out);
}

void ImageEditor::placeDialog(View& v) {
    std::string out;
    if (!ed::pickFile(out, startDir(), "Images", "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif")) return;
    img::Document src;
    std::string err;
    if (!src.load(out, err)) { m_d.status = err; return; }
    cancelGesture(v);
    const img::Pixels px = src.flattened();
    v.doc.checkpoint("Place");
    v.doc.insertLayer(fs::path(out).stem().string(), px, src.width(), src.height(),
                      (v.doc.width() - src.width()) / 2, (v.doc.height() - src.height()) / 2);
    m_d.status = "Placed " + src.title() + " on a layer of its own -- Ctrl+T scales it.";
}

bool ImageEditor::save(View& v, bool as) {
    std::string path = v.doc.path();
    if (as || path.empty() || !saveable(path)) {
        const std::string dir = path.empty() ? startDir() : fs::path(path).parent_path().generic_string();
        const std::string ext = v.doc.layerCount() > 1 && !as ? ".psd" : ".png";
        const std::string name = path.empty() ? "untitled" + ext : fs::path(path).stem().string() + ext;
        std::string out;
        if (!ed::saveFile(out, dir, name, "Images (png, jpg, tga, bmp, psd)", "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd", "png"))
            return false;
        path = out;
    }
    std::string err;
    if (!v.doc.save(path, err)) { m_d.status = "Not saved: " + err; return false; }
    m_lastDir = fs::path(path).parent_path().generic_string();
    v.closeArmed = false;
    std::string msg = "Saved " + v.doc.title();
    const std::string e = lower(fs::path(path).extension().string());
    if (e == ".psd") {
        if (v.doc.layerCount() > 1) msg += " with its " + std::to_string(v.doc.layerCount()) + " layers";
    } else if (v.doc.layerCount() > 1) {
        msg += " (" + std::to_string(v.doc.layerCount()) + " layers flattened into the file; they stay here -- .psd keeps them)";
    }
    if (e == ".jpg" || e == ".jpeg") msg += " -- JPEG keeps no transparency";
    m_d.status = msg;
    if (!projectDir().empty() && lower(path).rfind(lower(projectDir()), 0) == 0 &&
        std::find(m_images.begin(), m_images.end(), path) == m_images.end())
        m_scannedFor = "\x01";   // a new picture in the project: list it
    return true;
}

void ImageEditor::revert(View& v) {
    if (v.doc.path().empty()) return;
    cancelGesture(v);
    if (m_xf.on) transformCancel(v);
    img::Document fresh;
    std::string err;
    if (!fresh.load(v.doc.path(), err)) { m_d.status = err; return; }
    v.doc = std::move(fresh);
    v.fitPending = true;
    v.shape = 0;      // upload() makes a new texture for it
    v.selVer = 0;
    v.thumbVer = 0;
    m_d.status = "Reverted to the saved " + v.doc.title() + ".";
}

void ImageEditor::requestClose(int i) {
    View& v = *m_views[std::size_t(i)];
    if (v.doc.modified() && !v.closeArmed) {
        v.closeArmed = true;
        m_cur = i;
        m_selectTab = i;
        m_d.status = v.doc.title() + " has unsaved changes: close it again to let them go.";
        return;
    }
    closeView(i);
}

void ImageEditor::closeView(int i) {
    View& v = *m_views[std::size_t(i)];
    if (i == m_cur) {
        cancelGesture(v);
        if (m_xf.on) transformCancel(v);
        lassoCancel();
        cropCancel();
        if (m_pane != Pane::None) closePane(v, false);
    }
    if (m_text.on && m_text.viewId == v.id) m_text.on = false;
    if (v.tex) m_retired.push_back(v.tex);
    for (fitzel::rhi::TextureHandle t : v.thumbs) m_retired.push_back(t);
    m_views.erase(m_views.begin() + i);
    if (m_cur >= int(m_views.size())) m_cur = int(m_views.size()) - 1;
    else if (i < m_cur) --m_cur;
    m_selectTab = m_cur;
}

void ImageEditor::rememberColor(const img::Color& c) {
    auto same = [&](const img::Color& o) {
        return std::fabs(o.r - c.r) < 0.004f && std::fabs(o.g - c.g) < 0.004f && std::fabs(o.b - c.b) < 0.004f &&
               std::fabs(o.a - c.a) < 0.004f;
    };
    m_recent.erase(std::remove_if(m_recent.begin(), m_recent.end(), same), m_recent.end());
    m_recent.insert(m_recent.begin(), c);
    if (m_recent.size() > 15) m_recent.resize(15);
}

void ImageEditor::copy(View& v, bool merged, bool cut) {
    m_clip = v.doc.copy(merged);
    if (!m_clip.valid()) { m_d.status = "Nothing to copy."; return; }
    writeClipboard(m_clip);
    m_clipSeq = clipboardSeq();
    if (cut) v.doc.clearSelection();
    m_d.status = std::string(cut ? "Cut " : "Copied ") + std::to_string(m_clip.w) + " x " + std::to_string(m_clip.h) + " px.";
}

void ImageEditor::paste(View* v, bool asNew) {
    img::Clip c;
    // A picture another program put on the clipboard since our own copy wins;
    // otherwise our own (it keeps the alpha and the place it came from).
    const bool fromOs = clipboardHasImage() && clipboardSeq() != m_clipSeq && readClipboard(c);
    if (!fromOs) c = m_clip;
    if (!c.valid()) { m_d.status = "The clipboard has no picture."; return; }
    if (asNew || !v) {
        auto nv = std::make_unique<View>();
        nv->doc.adopt(c.w, c.h, c.px, "Pasted");
        nv->id = m_nextId++;
        m_views.push_back(std::move(nv));
        m_cur = int(m_views.size()) - 1;
        m_selectTab = m_cur;
        m_d.status = "Pasted as a new picture, " + std::to_string(c.w) + " x " + std::to_string(c.h) + ".";
        return;
    }
    if (fromOs) { c.x = (v->doc.width() - c.w) / 2; c.y = (v->doc.height() - c.h) / 2; }
    cancelGesture(*v);
    v->doc.paste(c);
    m_d.status = "Pasted onto a new layer -- the Move tool (V) puts it in place, Ctrl+T scales it.";
}

void ImageEditor::selectLayer(View& v, int i) {
    if (i == v.doc.active()) return;
    cancelGesture(v);
    if (m_xf.on) transformCommit(v);
    v.doc.setActive(i);
}

void ImageEditor::openFilter(View& v, img::Filter f) {
    cancelGesture(v);
    if (m_xf.on) transformCommit(v);
    if (m_pane != Pane::None) closePane(v, false);
    const img::FilterInfo& fi = img::filterInfo(f);
    const img::Color pair[2] = {m_fg, m_bg};
    if (fi.params == 0) {
        v.doc.filter(f, nullptr, m_seed, pair);
        m_d.status = std::string(fi.name) + (v.doc.hasSelection() ? " (inside the selection)." : ".");
        m_hasLastFilter = true;
        m_lastFilter = f;
        return;
    }
    m_pane = Pane::Filter;
    m_paneOpened = false;
    m_filter = f;
    m_preview = true;
    for (int k = 0; k < 3; ++k) m_fp[k] = fi.def[k];
    // The same filter again starts where it was left.
    if (m_hasLastFilter && m_lastFilter == f) for (int k = 0; k < 3; ++k) m_fp[k] = m_lastFp[k];
    v.doc.beginEdit(fi.name);
    v.doc.filter(f, m_fp, m_seed, pair);
}

void ImageEditor::repeatFilter(View& v) {
    if (!m_hasLastFilter) return;
    cancelGesture(v);
    const img::Color pair[2] = {m_fg, m_bg};
    const img::FilterInfo& fi = img::filterInfo(m_lastFilter);
    v.doc.filter(m_lastFilter, fi.params ? m_lastFp : nullptr, m_seed, pair);
    m_d.status = std::string(fi.name) + " again.";
}

void ImageEditor::openCurves(View& v) {
    cancelGesture(v);
    if (m_xf.on) transformCommit(v);
    if (m_pane != Pane::None) closePane(v, false);
    m_pane = Pane::Curves;
    m_paneOpened = false;
    m_preview = true;
    m_curveSel = -1;
    m_curveDrag = -1;
    for (auto& c : m_curve) c = {{0.0f, 0.0f}, {255.0f, 255.0f}};
    img::histogram(v.doc.flattened(), v.doc.width(), v.doc.height(), v.doc.selection(), m_hist);
    v.doc.beginEdit("Curves");
}

void ImageEditor::applyCurvesPreview(View& v) {
    img::Curves c;
    for (int k = 0; k < 4; ++k) img::curveTable(m_curve[k], c.lut[k]);
    v.doc.curves(c);
}

void ImageEditor::closePane(View& v, bool apply) {
    if (m_pane == Pane::Filter || m_pane == Pane::Curves) {
        if (apply && m_pane == Pane::Filter) {
            m_hasLastFilter = true;
            m_lastFilter = m_filter;
            for (int k = 0; k < 3; ++k) m_lastFp[k] = m_fp[k];
        }
        if (apply && !m_preview) {
            // Previewing was switched off: the result is not in the layer yet.
            const img::Color pair[2] = {m_fg, m_bg};
            if (m_pane == Pane::Filter) v.doc.filter(m_filter, m_fp, m_seed, pair); else applyCurvesPreview(v);
        }
        if (apply) v.doc.endEdit(); else v.doc.cancelEdit();
    }
    m_pane = Pane::None;
}

// --- The window -----------------------------------------------------------------------

void ImageEditor::panel(bool& show) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    // Textures retired last frame were drawn by then; now they can go.
    if (!m_retired.empty()) {
        for (fitzel::rhi::TextureHandle t : m_retired) gpu.destroy(t);
        m_retired.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(m_firstW, m_firstH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Image editor", &show, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                                                 ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    if (!m_checker) {
        const std::uint8_t px[16] = {204, 204, 204, 255, 153, 153, 153, 255, 153, 153, 153, 255, 204, 204, 204, 255};
        fitzel::rhi::TextureDesc td;
        td.format = fitzel::rhi::Format::RGBA8;
        td.width = td.height = 2;
        td.sampler = fitzel::rhi::SamplerDesc::linearRepeat();
        td.sampler.minFilter = td.sampler.magFilter = fitzel::rhi::Filter::Nearest;
        m_checker = gpu.createTexture(td, px, fitzel::rhi::Format::RGBA8);
    }
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused) m_focusFrame = ImGui::GetFrameCount();

    // Pictures dropped on the window open in tabs of their own.
    if (m_d.drops && !m_d.drops->empty() && m_d.dropX && m_d.dropY) {
        const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        if (*m_d.dropX >= p.x && *m_d.dropX <= p.x + sz.x && *m_d.dropY >= p.y && *m_d.dropY <= p.y + sz.y) {
            const std::vector<std::string> files = *m_d.drops;
            m_d.drops->clear();
            for (const std::string& f : files) {
                if (img::isImageFile(f)) open(f);
                else m_d.status = fs::path(f).filename().string() + " is not a picture the image editor reads.";
            }
        }
    }
    if (m_scannedFor != projectDir()) scanImages();

    menuBar();
    if (focused) shortcuts(cur());

    if (m_views.empty()) {
        welcome();
    } else {
        if (ImGui::BeginTabBar("##pictures", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int i = 0; i < int(m_views.size()); ++i) {
                View& w = *m_views[std::size_t(i)];
                char zoomTxt[32];
                std::snprintf(zoomTxt, sizeof zoomTxt, " @ %.0f%%", w.zoom * 100.0f);
                const std::string label = w.doc.title() + zoomTxt + "###pic" + std::to_string(w.id);
                bool open = true;
                ImGuiTabItemFlags fl = w.doc.modified() ? ImGuiTabItemFlags_UnsavedDocument : 0;
                if (m_selectTab == i) fl |= ImGuiTabItemFlags_SetSelected;
                if (ImGui::BeginTabItem(label.c_str(), &open, fl)) {
                    if (m_cur != i && m_selectTab < 0) {
                        if (View* o = cur()) {
                            cancelGesture(*o);
                            if (m_xf.on) transformCommit(*o);
                            if (m_pane != Pane::None) closePane(*o, false);
                        }
                        lassoCancel();
                        cropCancel();
                        m_cur = i;
                    }
                    ImGui::EndTabItem();
                }
                ImGui::SetItemTooltip("%s", w.doc.path().empty() ? "Not saved yet" : w.doc.path().c_str());
                if (!open) { requestClose(i); break; }
            }
            ImGui::EndTabBar();
        }
        m_selectTab = -1;
        if (View* v = cur()) {
            if (v->closeArmed) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.2f, 0.15f, 1.0f));
                const bool gone = bigButton("Close without saving");
                ImGui::PopStyleColor();
                if (gone) { closeView(m_cur); ImGui::End(); return; }
                ImGui::SameLine();
                if (bigButton("Keep it open")) v->closeArmed = false;
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ui::hint("%s has unsaved changes.", v->doc.title().c_str());
            }
            optionsBar(*v);
            ImGui::Separator();
            const float statusH = ImGui::GetFrameHeightWithSpacing() + 4.0f;
            const float side = std::round(em() * 2.1f);
            const ImGuiStyle& st = ImGui::GetStyle();
            const float toolW = side * 2.0f + st.ItemSpacing.x + st.WindowPadding.x * 2.0f;
            const float availW = ImGui::GetContentRegionAvail().x;
            const float rightW = std::clamp(availW * 0.24f, em() * 15.0f, em() * 19.0f);
            ImGui::BeginChild("##tools", ImVec2(toolW, -statusH), ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar);
            toolBar();
            fgBg();
            ImGui::EndChild();
            ImGui::SameLine();
            const float canvasW = ImGui::GetContentRegionAvail().x - rightW - st.ItemSpacing.x;
            ImGui::BeginChild("##canvas", ImVec2(std::max(100.0f, canvasW), -statusH), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            canvas(*v);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##right", ImVec2(0.0f, -statusH), ImGuiChildFlags_None);
            if (ImGui::BeginTabBar("##colortabs")) {
                if (ImGui::BeginTabItem("Color")) { colorPanel(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Swatches")) { swatchesPanel(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::Spacing();
            if (ImGui::BeginTabBar("##layertabs")) {
                if (ImGui::BeginTabItem("Layers")) { layersPanel(*v); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("History")) { historyPanel(*v); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            statusLine(*v);
        }
    }
    dialog(cur());
    ImGui::End();
    // While the window has the keyboard, claim it the way a text field does:
    // every editor shortcut (scene undo, Q/W/E, V, G, camera keys ...) is read
    // before the next UI frame and stands aside for io.WantTextInput.
    if (focused || m_pane != Pane::None) ImGui::GetIO().WantTextInput = true;
}

void ImageEditor::shortcuts(View* v) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;            // a field of this window is being typed into
    if (m_pane != Pane::None) return;        // the dialog has the keys
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    if (ctrl) {
        if (key(ImGuiKey_O)) openDialog();
        if (key(ImGuiKey_N) && !shift) { m_pane = Pane::New; m_paneOpened = false; }
        if (key(ImGuiKey_V) && shift) paste(v, true);
        if (!v) return;
        img::Document& d = v->doc;
        if (key(ImGuiKey_Z)) {
            cancelGesture(*v);
            if (m_xf.on) { transformCancel(*v); return; }
            if (shift) d.redo(); else d.undo();
        }
        if (key(ImGuiKey_Y)) { cancelGesture(*v); if (m_xf.on) transformCancel(*v); d.redo(); }
        if (key(ImGuiKey_S)) save(*v, shift);
        if (key(ImGuiKey_W)) { requestClose(m_cur); return; }
        if (key(ImGuiKey_C) && !alt) copy(*v, shift, false);
        if (key(ImGuiKey_X)) copy(*v, false, true);
        if (key(ImGuiKey_V) && !shift) paste(v, false);
        if (key(ImGuiKey_A)) d.selectAll();
        if (key(ImGuiKey_D)) { if (shift) d.reselect(); else d.deselect(); }
        if (key(ImGuiKey_I) && shift) d.invertSelection();
        if (key(ImGuiKey_I) && !shift && alt) { m_pane = Pane::Resize; m_paneOpened = false; m_resW = d.width(); m_resH = d.height(); }
        if (key(ImGuiKey_I) && !shift && !alt) openFilter(*v, img::Filter::Invert);
        if (key(ImGuiKey_C) && alt) { m_pane = Pane::Canvas; m_paneOpened = false; m_resW = d.width(); m_resH = d.height(); }
        if (key(ImGuiKey_T)) startTransform(*v);
        if (key(ImGuiKey_J)) { cancelGesture(*v); d.layerViaCopy(shift); }
        if (key(ImGuiKey_N) && shift) d.addLayer();
        if (key(ImGuiKey_E)) { if (shift) d.mergeVisible(); else d.mergeDown(); }
        if (key(ImGuiKey_RightBracket)) d.moveLayerTo(d.active(), shift ? d.layerCount() - 1 : d.active() + 1);
        if (key(ImGuiKey_LeftBracket)) d.moveLayerTo(d.active(), shift ? 0 : d.active() - 1);
        if (key(ImGuiKey_L)) {
            if (shift && alt) openFilter(*v, img::Filter::AutoContrast);
            else if (shift) openFilter(*v, img::Filter::AutoTone);
            else openFilter(*v, img::Filter::Levels);
        }
        if (key(ImGuiKey_M)) openCurves(*v);
        if (key(ImGuiKey_U)) openFilter(*v, shift ? img::Filter::Desaturate : img::Filter::HueSaturation);
        if (key(ImGuiKey_B)) openFilter(*v, img::Filter::ColorBalance);
        if (key(ImGuiKey_F)) repeatFilter(*v);
        if (key(ImGuiKey_H)) m_showEdges = !m_showEdges;
        if (key(ImGuiKey_0)) v->fitPending = true;
        if (key(ImGuiKey_1)) zoomAt(*v, 1.0f, m_center);
        if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) zoomAt(*v, nextZoom(v->zoom, +1), m_center);
        if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) zoomAt(*v, nextZoom(v->zoom, -1), m_center);
        if (key(ImGuiKey_Backspace)) { d.fillSelection(m_bg); }
        return;
    }
    if (key(ImGuiKey_Escape)) {
        if (v && m_xf.on) transformCancel(*v);
        else if (m_crop.on) cropCancel();
        else if (m_lasso.on) lassoCancel();
        else if (m_text.on) textEnd(false);
        else if (v) cancelGesture(*v);
    }
    if (!v) return;
    if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) {
        if (m_xf.on) transformCommit(*v);
        else if (m_crop.on) cropCommit(*v);
        else if (m_lasso.on) lassoClose(*v);
    }
    if (shift && key(ImGuiKey_F5)) { m_pane = Pane::Fill; m_paneOpened = false; return; }
    if (shift && key(ImGuiKey_F6) && v->doc.hasSelection()) {
        m_pane = Pane::Modify; m_paneOpened = false; m_modOp = img::SelModify::Feather; return;
    }
    if (key(ImGuiKey_F12)) revert(*v);
    if (key(ImGuiKey_F2)) { m_renaming = v->doc.active(); m_renameFocus = true; }
    if (alt && (key(ImGuiKey_Backspace) || key(ImGuiKey_Delete))) { v->doc.fillSelection(m_fg); rememberColor(m_fg); return; }
    // Tools by letter; Shift+letter switches between the tool's kinds, as in Photoshop.
    for (int t = 0; t < int(Tool::Count); ++t) {
        if (!key(kTools[t].imkey)) continue;
        const Tool tool = Tool(t);
        if (shift && m_tool == tool) {
            if (tool == Tool::Select) m_ellipse = !m_ellipse;
            if (tool == Tool::Lasso) { lassoCancel(); m_polyLasso = !m_polyLasso; }
            if (tool == Tool::Blur) m_sharpen = 1 - m_sharpen;
            if (tool == Tool::Dodge) m_tone = (m_tone + 1) % 4;
            if (tool == Tool::Shape) m_shape = (m_shape + 1) % 5;
            if (tool == Tool::Gradient) { setTool(Tool::Fill); break; }
        } else if (shift && tool == Tool::Gradient && m_tool == Tool::Fill) {
            setTool(Tool::Gradient);
        } else {
            setTool(tool);
        }
        break;
    }
    if (key(ImGuiKey_X)) std::swap(m_fg, m_bg);
    if (key(ImGuiKey_D)) { m_fg = {0, 0, 0, 1}; m_bg = {1, 1, 1, 1}; }
    if (key(ImGuiKey_Slash)) v->doc.setLockAlpha(v->doc.active(), !v->doc.layer(v->doc.active()).lockAlpha);
    if (shift) {
        if (key(ImGuiKey_LeftBracket)) m_brush.hardness = std::max(0.0f, m_brush.hardness - 0.25f);
        if (key(ImGuiKey_RightBracket)) m_brush.hardness = std::min(1.0f, m_brush.hardness + 0.25f);
    } else {
        if (key(ImGuiKey_LeftBracket)) m_brush.radius = std::max(0.5f, m_brush.radius / 1.25f);
        if (key(ImGuiKey_RightBracket)) m_brush.radius = std::min(500.0f, m_brush.radius * 1.25f);
    }
    // Number keys set the tool's opacity: 1 = 10 % ... 0 = 100 %.
    for (int k = 0; k <= 9; ++k)
        if (key(ImGuiKey(ImGuiKey_0 + k))) {
            const float o = k == 0 ? 1.0f : k * 0.1f;
            if (m_tool == Tool::Blur || m_tool == Tool::Dodge) m_retouch = o;
            else if (m_tool == Tool::Fill || m_tool == Tool::Gradient) m_fillOpacity = o;
            else m_brush.opacity = o;
        }
    if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) zoomAt(*v, nextZoom(v->zoom, +1), m_center);
    if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) zoomAt(*v, nextZoom(v->zoom, -1), m_center);
    if (key(ImGuiKey_Delete) || (key(ImGuiKey_Backspace) && !m_lasso.on && !alt)) v->doc.clearSelection();
    if (m_lasso.on && key(ImGuiKey_Backspace) && m_lasso.pts.size() > 1) m_lasso.pts.pop_back();
    // Arrows: the Move tool moves pixels, the selection tools the outline.
    const int s = shift ? 10 * std::max(1, m_nudge) : std::max(1, m_nudge);
    int dx = 0, dy = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx -= s;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx += s;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy -= s;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy += s;
    if (dx || dy) nudge(*v, dx, dy);
}

void ImageEditor::nudge(View& v, int dx, int dy) {
    img::Document& d = v.doc;
    if (m_xf.on) {
        m_xf.xf.tx += float(dx);
        m_xf.xf.ty += float(dy);
        transformApply(v);
    } else if (m_tool == Tool::Move) {
        d.moveSelected(dx, dy);
    } else if ((m_tool == Tool::Select || m_tool == Tool::Lasso || m_tool == Tool::Wand) && d.hasSelection()) {
        auto base = d.selectionShared();
        d.checkpoint("Move selection", "nudgesel");
        d.shiftSelection(base, dx, dy);
    }
}

void ImageEditor::menuBar() {
    if (!ImGui::BeginMenuBar()) return;
    View* v = cur();
    const bool has = v != nullptr;
    const bool busy = m_pane != Pane::None;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New...", "Ctrl+N")) { m_pane = Pane::New; m_paneOpened = false; }
        if (ImGui::MenuItem("Open...", "Ctrl+O")) openDialog();
        if (ImGui::BeginMenu("Open from project", !m_images.empty())) {
            ui::searchBox("##msearch", m_search, sizeof m_search, "Search...");
            int shown = 0;
            const std::string base = projectDir();
            for (const std::string& f : m_images) {
                const std::string rel = f.size() > base.size() ? f.substr(base.size() + 1) : f;
                if (!ui::icontains(rel.c_str(), m_search)) continue;
                if (++shown > 60) { ui::hint("... search for more"); break; }
                if (ImGui::MenuItem(rel.c_str())) open(f);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Paste as new picture", "Shift+Ctrl+V")) paste(v, true);
        if (ImGui::MenuItem("Place as layer...", nullptr, false, has && !busy)) placeDialog(*v);
        ImGui::Separator();
        if (ImGui::MenuItem("Close", "Ctrl+W", false, has)) requestClose(m_cur);
        if (ImGui::MenuItem("Save", "Ctrl+S", false, has && !busy)) save(*v, false);
        if (ImGui::MenuItem("Save as...", "Shift+Ctrl+S", false, has && !busy)) save(*v, true);
        ImGui::SetItemTooltip("PNG, JPG, TGA, BMP -- or PSD, which keeps the layers.");
        if (ImGui::MenuItem("Revert", "F12", false, has && !busy && !v->doc.path().empty())) revert(*v);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit", has && !busy)) {
        img::Document& d = v->doc;
        const std::string ul = d.canUndo() ? "Undo " + d.undoLabel() : std::string("Undo");
        const std::string rl = d.canRedo() ? "Redo " + d.redoLabel() : std::string("Redo");
        if (ImGui::MenuItem(ul.c_str(), "Ctrl+Z", false, d.canUndo() || m_xf.on)) {
            cancelGesture(*v);
            if (m_xf.on) transformCancel(*v); else d.undo();
        }
        if (ImGui::MenuItem(rl.c_str(), "Shift+Ctrl+Z", false, d.canRedo())) { cancelGesture(*v); d.redo(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X")) copy(*v, false, true);
        if (ImGui::MenuItem("Copy", "Ctrl+C")) copy(*v, false, false);
        if (ImGui::MenuItem("Copy merged", "Shift+Ctrl+C")) copy(*v, true, false);
        if (ImGui::MenuItem("Paste", "Ctrl+V")) paste(v, false);
        if (ImGui::MenuItem("Clear", "Delete")) d.clearSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Fill...", "Shift+F5")) { m_pane = Pane::Fill; m_paneOpened = false; }
        if (ImGui::MenuItem("Fill with foreground", "Alt+Backspace")) { d.fillSelection(m_fg); rememberColor(m_fg); }
        if (ImGui::MenuItem("Fill with background", "Ctrl+Backspace")) d.fillSelection(m_bg);
        if (ImGui::MenuItem("Stroke...", nullptr, false, d.hasSelection())) { m_pane = Pane::Stroke; m_paneOpened = false; }
        ImGui::Separator();
        if (ImGui::MenuItem("Free transform", "Ctrl+T")) startTransform(*v);
        if (ImGui::BeginMenu("Transform")) {
            auto quick = [&](const char* label, float sx, float sy, float rot) {
                cancelGesture(*v);
                if (m_xf.on) transformCommit(*v);
                const img::Rect r = d.transformBounds();
                img::Xform x;
                x.px = (r.x0 + r.x1) * 0.5f; x.py = (r.y0 + r.y1) * 0.5f;
                x.sx = sx; x.sy = sy; x.rot = rot;
                snapToPixels(x, r);
                d.beginEdit(label);
                d.transformSelected(x);
                d.endEdit();
            };
            ui::hint(d.hasSelection() ? "What is selected on this layer" : "The whole layer");
            if (ImGui::MenuItem("Rotate 180")) quick("Rotate 180", 1, 1, 180);
            if (ImGui::MenuItem("Rotate 90 clockwise")) quick("Rotate 90 clockwise", 1, 1, 90);
            if (ImGui::MenuItem("Rotate 90 counter-clockwise")) quick("Rotate 90 counter-clockwise", 1, 1, -90);
            ImGui::Separator();
            if (ImGui::MenuItem("Flip horizontal")) quick("Flip horizontal", -1, 1, 0);
            if (ImGui::MenuItem("Flip vertical")) quick("Flip vertical", 1, -1, 0);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Image", has && !busy)) {
        img::Document& d = v->doc;
        if (ImGui::BeginMenu("Adjustments")) {
            if (ImGui::MenuItem("Curves...", "Ctrl+M")) openCurves(*v);
            ImGui::SetItemTooltip("Bend the tones with a curve: lift the shadows, tame the highlights, per channel.");
            static const struct { img::Filter f; const char* key; } kAdjust[] = {
                {img::Filter::BrightnessContrast, nullptr}, {img::Filter::Levels, "Ctrl+L"}, {img::Filter::Exposure, nullptr},
                {img::Filter::Vibrance, nullptr}, {img::Filter::HueSaturation, "Ctrl+U"}, {img::Filter::ColorBalance, "Ctrl+B"},
                {img::Filter::PhotoFilter, nullptr}, {img::Filter::Count, nullptr},
                {img::Filter::Invert, "Ctrl+I"}, {img::Filter::Posterize, nullptr}, {img::Filter::Threshold, nullptr},
                {img::Filter::GradientMap, nullptr}, {img::Filter::Desaturate, "Shift+Ctrl+U"}, {img::Filter::Count, nullptr},
                {img::Filter::AutoTone, "Shift+Ctrl+L"}, {img::Filter::AutoContrast, "Alt+Shift+Ctrl+L"},
            };
            for (const auto& a : kAdjust) {
                if (a.f == img::Filter::Count) { ImGui::Separator(); continue; }
                const img::FilterInfo& fi = img::filterInfo(a.f);
                const std::string label = std::string(fi.name) + (fi.params ? "..." : "");
                if (ImGui::MenuItem(label.c_str(), a.key)) openFilter(*v, a.f);
                ImGui::SetItemTooltip("%s", fi.tip);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Image size...", "Alt+Ctrl+I")) { m_pane = Pane::Resize; m_paneOpened = false; m_resW = d.width(); m_resH = d.height(); }
        if (ImGui::MenuItem("Canvas size...", "Alt+Ctrl+C")) { m_pane = Pane::Canvas; m_paneOpened = false; m_resW = d.width(); m_resH = d.height(); }
        if (ImGui::BeginMenu("Image rotation")) {
            if (ImGui::MenuItem("180")) d.rotate180();
            if (ImGui::MenuItem("90 clockwise")) d.rotate90(true);
            if (ImGui::MenuItem("90 counter-clockwise")) d.rotate90(false);
            ImGui::Separator();
            if (ImGui::MenuItem("Flip canvas horizontal")) d.flipImage(true);
            if (ImGui::MenuItem("Flip canvas vertical")) d.flipImage(false);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Crop", nullptr, false, d.hasSelection())) { d.crop(d.selectionBounds()); v->fitPending = true; }
        ImGui::SetItemTooltip("Cut the picture down to the selection's box.");
        if (ImGui::MenuItem("Trim")) { d.trim(); v->fitPending = true; }
        ImGui::SetItemTooltip("Cut away the see-through border.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Layer", has && !busy)) {
        img::Document& d = v->doc;
        if (ImGui::BeginMenu("New")) {
            if (ImGui::MenuItem("Layer", "Shift+Ctrl+N")) d.addLayer();
            if (ImGui::MenuItem("Layer via copy", "Ctrl+J")) { cancelGesture(*v); d.layerViaCopy(false); }
            if (ImGui::MenuItem("Layer via cut", "Shift+Ctrl+J")) { cancelGesture(*v); d.layerViaCopy(true); }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Duplicate layer")) d.duplicateLayer();
        if (ImGui::MenuItem("Delete layer", nullptr, false, d.layerCount() > 1)) d.deleteLayer();
        if (ImGui::MenuItem("Rename layer...", "F2")) { m_renaming = d.active(); m_renameFocus = true; }
        ImGui::Separator();
        if (ImGui::BeginMenu("Arrange")) {
            const int a = d.active(), top = d.layerCount() - 1;
            if (ImGui::MenuItem("Bring to front", "Shift+Ctrl+]", false, a < top)) d.moveLayerTo(a, top);
            if (ImGui::MenuItem("Bring forward", "Ctrl+]", false, a < top)) d.moveLayerTo(a, a + 1);
            if (ImGui::MenuItem("Send backward", "Ctrl+[", false, a > 0)) d.moveLayerTo(a, a - 1);
            if (ImGui::MenuItem("Send to back", "Shift+Ctrl+[", false, a > 0)) d.moveLayerTo(a, 0);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Lock transparent pixels", "/", d.layer(d.active()).lockAlpha))
            d.setLockAlpha(d.active(), !d.layer(d.active()).lockAlpha);
        ImGui::Separator();
        if (ImGui::MenuItem("Merge down", "Ctrl+E", false, d.active() > 0)) d.mergeDown();
        if (ImGui::MenuItem("Merge visible", "Shift+Ctrl+E", false, d.layerCount() > 1)) d.mergeVisible();
        if (ImGui::MenuItem("Flatten image", nullptr, false, d.layerCount() > 1)) d.flatten();
        ImGui::Separator();
        if (ImGui::MenuItem("Flip layer horizontal")) d.flipLayer(true);
        if (ImGui::MenuItem("Flip layer vertical")) d.flipLayer(false);
        if (ImGui::MenuItem("Offset by half")) d.offsetLayer(d.width() / 2, d.height() / 2);
        ImGui::SetItemTooltip("Wraps the layer round by half its size -- a texture's seams come to the middle.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Select", has && !busy)) {
        img::Document& d = v->doc;
        if (ImGui::MenuItem("All", "Ctrl+A")) d.selectAll();
        if (ImGui::MenuItem("Deselect", "Ctrl+D", false, d.hasSelection())) d.deselect();
        if (ImGui::MenuItem("Reselect", "Shift+Ctrl+D", false, d.canReselect())) d.reselect();
        if (ImGui::MenuItem("Inverse", "Shift+Ctrl+I")) d.invertSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Load layer transparency")) d.selectLayerAlpha(img::SelOp::Replace);
        ImGui::SetItemTooltip("Select what the layer has painted (Ctrl+click its thumbnail).");
        if (ImGui::BeginMenu("Modify", d.hasSelection())) {
            static const struct { img::SelModify m; const char* name; const char* key; } kMods[] = {
                {img::SelModify::Border, "Border...", nullptr}, {img::SelModify::Smooth, "Smooth...", nullptr},
                {img::SelModify::Expand, "Expand...", nullptr}, {img::SelModify::Contract, "Contract...", nullptr},
                {img::SelModify::Feather, "Feather...", "Shift+F6"}};
            for (const auto& m : kMods)
                if (ImGui::MenuItem(m.name, m.key)) { m_pane = Pane::Modify; m_paneOpened = false; m_modOp = m.m; }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Filter", has && !busy)) {
        const std::string last = m_hasLastFilter ? std::string("Repeat ") + img::filterInfo(m_lastFilter).name : "Repeat last filter";
        if (ImGui::MenuItem(last.c_str(), "Ctrl+F", false, m_hasLastFilter)) repeatFilter(*v);
        ImGui::Separator();
        for (const char* menu : kFilterMenus) {
            if (!ImGui::BeginMenu(menu)) continue;
            for (int f = 0; f < int(img::Filter::Count); ++f) {
                const img::FilterInfo& fi = img::filterInfo(img::Filter(f));
                if (std::strcmp(fi.menu, menu) != 0) continue;
                const std::string label = std::string(fi.name) + (fi.params ? "..." : "");
                if (ImGui::MenuItem(label.c_str())) openFilter(*v, img::Filter(f));
                ImGui::SetItemTooltip("%s", fi.tip);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View", has)) {
        if (ImGui::MenuItem("Zoom in", "Ctrl++")) zoomAt(*v, nextZoom(v->zoom, +1), m_center);
        if (ImGui::MenuItem("Zoom out", "Ctrl+-")) zoomAt(*v, nextZoom(v->zoom, -1), m_center);
        if (ImGui::MenuItem("Fit on screen", "Ctrl+0")) v->fitPending = true;
        if (ImGui::MenuItem("100 %", "Ctrl+1")) zoomAt(*v, 1.0f, m_center);
        ImGui::Separator();
        ImGui::MenuItem("Pixel grid", nullptr, &m_pixelGrid);
        ImGui::SetItemTooltip("Lines between the pixels from 800 %% on.");
        ImGui::MenuItem("Selection edges", "Ctrl+H", &m_showEdges);
        ImGui::MenuItem("Tile preview", nullptr, &m_tilePreview);
        ImGui::SetItemTooltip("Shows the picture repeated around itself -- for checking a texture's seams.");
        if (ImGui::BeginMenu("Snap")) {
            for (int s : {1, 2, 4, 8, 16, 32, 64, 128}) {
                char label[32];
                std::snprintf(label, sizeof label, s == 1 ? "Whole pixels" : "%d px grid", s);
                if (ImGui::MenuItem(label, nullptr, m_snap == s)) m_snap = s;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    // Undo, redo and the zoom at the right end of the menu bar.
    if (v) {
        img::Document& d = v->doc;
        const float side = ImGui::GetFrameHeight();
        const float w = side * 8.0f + ImGui::CalcTextSize("0000 %").x + ImGui::GetStyle().ItemSpacing.x * 10.0f + em() * 1.5f;
        const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w;
        if (x > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(x);
            ImGui::BeginDisabled(busy || (!d.canUndo() && !m_xf.on));
            const std::string ut = d.canUndo() ? "Undo " + d.undoLabel() + "  (Ctrl+Z)" : std::string("Undo  (Ctrl+Z)");
            if (iconButton("##undo", Icon::Undo, ut.c_str())) { cancelGesture(*v); if (m_xf.on) transformCancel(*v); else d.undo(); }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(busy || !d.canRedo());
            const std::string rt = d.canRedo() ? "Redo " + d.redoLabel() + "  (Shift+Ctrl+Z)" : std::string("Redo  (Shift+Ctrl+Z)");
            if (iconButton("##redo", Icon::Redo, rt.c_str())) { cancelGesture(*v); d.redo(); }
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(em() * 0.5f, 1.0f));
            if (iconButton("##zout", Icon::ZoomOut, "Zoom out  (Ctrl+-)")) zoomAt(*v, nextZoom(v->zoom, -1), m_center);
            char z[32];
            std::snprintf(z, sizeof z, v->zoom >= 1.0f ? "%.0f %%" : "%.1f %%", v->zoom * 100.0f);
            ImGui::TextUnformatted(z);
            if (iconButton("##zin", Icon::ZoomIn, "Zoom in  (Ctrl++). The mouse wheel zooms where the pointer is.")) zoomAt(*v, nextZoom(v->zoom, +1), m_center);
            if (iconButton("##fit", Icon::Fit, "Fit on screen  (Ctrl+0)")) v->fitPending = true;
            if (iconButton("##one", Icon::OneToOne, "100 %: one picture pixel per screen pixel  (Ctrl+1)")) zoomAt(*v, 1.0f, m_center);
            ImGui::Dummy(ImVec2(em() * 0.5f, 1.0f));
            if (iconButton("##grid", Icon::Grid, "Pixel grid: lines between the pixels from 800 % on", m_pixelGrid)) m_pixelGrid = !m_pixelGrid;
            if (iconButton("##tile", Icon::Tile, "Tile preview: the picture repeated round itself, for a texture's seams", m_tilePreview))
                m_tilePreview = !m_tilePreview;
        }
    }
    ImGui::EndMenuBar();
}

// The New picture controls -- in the welcome page and the New dialog. True when made.
static bool newControls(int& w, int& h, int& fill, const img::Color& fg, std::function<void(int, int, img::Color)> make) {
    const float bw = across(3);
    struct Preset { const char* label; int w, h; };
    static const Preset presets[] = {{"256", 256, 256}, {"512", 512, 512}, {"1024", 1024, 1024},
                                     {"2048", 2048, 2048}, {"4096", 4096, 4096}, {"1920 x 1080", 1920, 1080}};
    for (int i = 0; i < 6; ++i) {
        if (i % 3) ImGui::SameLine();
        if (toggle(presets[i].label, w == presets[i].w && h == presets[i].h, bw)) { w = presets[i].w; h = presets[i].h; }
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(em() * 9.0f);
    ImGui::InputInt("Width", &w, 1, 64);
    ImGui::SetNextItemWidth(em() * 9.0f);
    ImGui::InputInt("Height", &h, 1, 64);
    w = std::clamp(w, 1, 16384);
    h = std::clamp(h, 1, 16384);
    ImGui::Spacing();
    ImGui::TextUnformatted("Background");
    static const char* fills[] = {"White", "Black", "Transparent", "Foreground"};
    const float fw = across(2);
    for (int i = 0; i < 4; ++i) {
        if (i % 2) ImGui::SameLine();
        if (toggle(fills[i], fill == i, fw)) fill = i;
    }
    ImGui::Spacing();
    if (bigButton("Create", nullptr, -1.0f)) {
        const img::Color c = fill == 0 ? img::Color{1, 1, 1, 1} : fill == 1 ? img::Color{0, 0, 0, 1}
                           : fill == 2 ? img::Color{0, 0, 0, 0} : fg;
        make(w, h, c);
        return true;
    }
    return false;
}

void ImageEditor::welcome() {
    ImGui::Spacing();
    ui::title("Image editor");
    ui::hint("Paint, retouch and resize the pictures a project is made of -- laid out like Photoshop, with its\n"
             "tools, menus and shortcuts. Open one below, drop a file on this window, or start a new one.\n"
             "Saving writes the file; materials using it update. A .psd keeps its layers.");
    ImGui::Spacing();
    const float colW = std::max(360.0f, em() * 22.0f);
    ImGui::BeginChild("##new", ImVec2(colW, 0.0f), ImGuiChildFlags_Borders);
    ui::sectionText("New picture");
    newControls(m_newW, m_newH, m_newFill, m_fg, [&](int w, int h, img::Color c) { newImage(w, h, c); });
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##open", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    ui::sectionText("Open");
    if (bigButton("Open file...", "PNG, JPG, TGA, BMP, PSD (with its layers), GIF (the first picture)", across(2))) openDialog();
    ImGui::SameLine();
    if (bigButton("Paste from clipboard", "A screenshot or a picture copied in another program.", across(1))) paste(nullptr, true);
    ImGui::Spacing();
    if (projectDir().empty()) {
        ui::hint("No project open: its pictures would be listed here.");
    } else {
        ImGui::AlignTextToFramePadding();
        ui::title("Pictures in this project (%d)", int(m_images.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan")) m_scannedFor = "\x01";
        ui::searchBox("##search", m_search, sizeof m_search, "Search by name or folder...");
        ImGui::BeginChild("##list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        const std::string base = projectDir();
        for (const std::string& f : m_images) {
            const std::string rel = f.size() > base.size() ? f.substr(base.size() + 1) : f;
            if (!ui::icontains(rel.c_str(), m_search)) continue;
            if (ImGui::Selectable(rel.c_str(), false, 0, ImVec2(0.0f, em() * 1.6f))) open(f);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

// --- The options bar ----------------------------------------------------------------------

void ImageEditor::optionsBar(View& v) {
    img::Document& d = v.doc;
    Flow f{ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x};
    const float barY = ImGui::GetCursorPosY();
    // The tool itself first, as Photoshop's bar starts with it.
    {
        f.next(barRow());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float s = barRow();
        ImGui::Dummy(ImVec2(s, s));
        static const Icon kIcons[] = {Icon::Move, Icon::Marquee, Icon::Lasso, Icon::Wand, Icon::Crop, Icon::Eyedropper,
                                      Icon::Brush, Icon::Clone, Icon::Eraser, Icon::Gradient, Icon::Bucket, Icon::Blur,
                                      Icon::Dodge, Icon::Text, Icon::Shape, Icon::Hand, Icon::Zoom};
        drawIcon(ImGui::GetWindowDrawList(), kIcons[int(m_tool)], ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.7f,
                 ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SetItemTooltip("%s", kTools[int(m_tool)].name);
        f.rule();
    }
    auto selModes = [&] {
        static const Icon icons[] = {Icon::SelReplace, Icon::SelAdd, Icon::SelSubtract, Icon::SelIntersect};
        static const char* tips[] = {"New selection", "Add to the selection  (hold Shift)",
                                     "Subtract from the selection  (hold Alt)", "Intersect with the selection  (Shift+Alt)"};
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            if (flowIcon(f, "##selop", icons[i], tips[i], int(m_selOp) == i)) m_selOp = img::SelOp(i);
            ImGui::PopID();
        }
        f.rule();
    };
    auto snapStep = [&] {
        static const int steps[] = {1, 2, 4, 8, 16, 32, 64, 128};
        int idx = 0;
        while (idx < 7 && steps[idx] < m_snap) ++idx;
        float fi = float(idx);
        char label[32];
        if (m_snap <= 1) std::snprintf(label, sizeof label, "pixels");
        else std::snprintf(label, sizeof label, "%d px", m_snap);
        if (flowStepper(f, "Snap", "snap", fi, 1.0f, 0.0f, 7.0f, label, "Corners, shapes, text and moves land on this grid.",
                        em() * 7.0f))
            m_snap = steps[std::clamp(int(fi), 0, 7)];
    };
    auto constrain = [&](const char* what) { flowCheck(f, what, &m_constrain, "The same as holding Shift."); };
    auto brushSize = [&] {
        const float r = m_brush.radius;
        const float step = r < 5 ? 0.5f : r < 20 ? 1.0f : r < 50 ? 5.0f : r < 200 ? 10.0f : 25.0f;
        float diam = r * 2.0f;
        if (flowStepper(f, "Size", "size", diam, step * 2.0f, 1.0f, 1000.0f, "%.0f px", "[ and ] change it too."))
            m_brush.radius = diam * 0.5f;
        flowPercent(f, "Hardness", "hard", m_brush.hardness, 10.0f, "Shift+[ and Shift+] change it too.");
    };
    auto steady = [&] {
        flowStepper(f, "Steady", "stab", m_stabilize, 2.0f, 0.0f, 120.0f, m_stabilize <= 0 ? "off" : "%.0f px",
                    "Stabiliser: the brush follows the pointer on a string this long,\n"
                    "so a shaking hand draws a calm line. 0 = off.");
        if (flowIcon(f, "##linemode", Icon::LineMode,
                     "Lines from click to click: each click paints a straight line from the last one -- no dragging.\n"
                     "Esc starts afresh. (Shift+click does it once with the normal brush.)", m_lineMode))
            m_lineMode = !m_lineMode;
        if (m_lineMode) constrain("45 degree steps");
    };

    const bool xfHere = m_xf.on && m_xf.viewId == v.id;
    switch (xfHere ? Tool::Count : m_tool) {   // a transform in progress shows its own options instead
    case Tool::Move:
        flowCheck(f, "Auto-select layer", &m_autoSelect, "A click takes the topmost layer that has paint under the pointer.");
        f.rule();
        if (flowIcon(f, "##xf", Icon::Transform, "Free transform  (Ctrl+T): scale, turn and flip with handles.", m_xf.on) && !m_xf.on)
            startTransform(v);
        f.rule();
        snapStep();
        {
            float n = float(m_nudge);
            if (flowStepper(f, "Step", "nudge", n, 1.0f, 1.0f, 256.0f, "%.0f px", "What one arrow key moves (Shift: ten times).",
                            em() * 6.5f))
                m_nudge = int(n);
        }
        if (flowIcon(f, "##ml", Icon::Left, "Move left  (Left arrow)")) nudge(v, -m_nudge, 0);
        if (flowIcon(f, "##mu", Icon::Up, "Move up  (Up arrow)")) nudge(v, 0, -m_nudge);
        if (flowIcon(f, "##md", Icon::Down, "Move down  (Down arrow)")) nudge(v, 0, m_nudge);
        if (flowIcon(f, "##mr", Icon::Right, "Move right  (Right arrow)")) nudge(v, m_nudge, 0);
        f.rule();
        constrain("Straight only");
        break;
    case Tool::Select:
        if (flowIcon(f, "##rect", Icon::Marquee, "Rectangle  (Shift+M)", !m_ellipse)) m_ellipse = false;
        if (flowIcon(f, "##ell", Icon::MarqueeEllipse, "Ellipse  (Shift+M)", m_ellipse)) m_ellipse = true;
        f.rule();
        selModes();
        snapStep();
        constrain(m_ellipse ? "Circle" : "Square");
        break;
    case Tool::Lasso:
        if (flowIcon(f, "##free", Icon::Lasso, "Freehand lasso: drag round it  (Shift+L)", !m_polyLasso)) { lassoCancel(); m_polyLasso = false; }
        if (flowIcon(f, "##poly", Icon::PolyLasso, "Polygonal lasso: click corner after corner; click the first point,\n"
                                                    "double-click or Enter closes it, Backspace takes the last corner back  (Shift+L)",
                     m_polyLasso)) { lassoCancel(); m_polyLasso = true; }
        f.rule();
        selModes();
        if (m_lasso.on && m_polyLasso) {
            char t[64];
            std::snprintf(t, sizeof t, "%d corners", int(m_lasso.pts.size()));
            flowText(f, t);
            if (flowIcon(f, "##lclose", Icon::Check, "Close the shape  (Enter)")) lassoClose(v);
            if (flowIcon(f, "##lcancel", Icon::Cancel, "Throw it away  (Esc)")) lassoCancel();
        }
        break;
    case Tool::Wand:
        selModes();
        flowPercent(f, "Tolerance", "tol", m_tolerance, 2.0f, "How far a colour may be from the clicked one.");
        flowCheck(f, "Contiguous", &m_contiguous, "Only pixels joined to the clicked one.");
        flowCheck(f, "Sample all layers", &m_sampleMerged);
        break;
    case Tool::Crop:
        if (m_crop.on && m_crop.viewId == v.id) {
            float x = std::round(std::min(m_crop.x0, m_crop.x1)), y = std::round(std::min(m_crop.y0, m_crop.y1));
            float w = std::round(std::fabs(m_crop.x1 - m_crop.x0)), h = std::round(std::fabs(m_crop.y1 - m_crop.y0));
            bool ch = false;
            ch |= flowStepper(f, "X", "cx", x, 1.0f, -16384.0f, 16384.0f, "%.0f", nullptr, em() * 6.5f);
            ch |= flowStepper(f, "Y", "cy", y, 1.0f, -16384.0f, 16384.0f, "%.0f", nullptr, em() * 6.5f);
            ch |= flowStepper(f, "W", "cw", w, 1.0f, 1.0f, 16384.0f, "%.0f", nullptr, em() * 6.5f);
            ch |= flowStepper(f, "H", "ch", h, 1.0f, 1.0f, 16384.0f, "%.0f", nullptr, em() * 6.5f);
            if (ch) { m_crop.x0 = x; m_crop.y0 = y; m_crop.x1 = x + w; m_crop.y1 = y + h; }
            f.rule();
            if (flowIcon(f, "##cok", Icon::Check, "Crop  (Enter)")) cropCommit(v);
            if (flowIcon(f, "##cno", Icon::Cancel, "Cancel  (Esc)")) cropCancel();
        } else {
            flowText(f, "Click two corners or drag a box.");
            if (d.hasSelection() && flowButton(f, "From selection", "Start the crop box at the selection's box.")) {
                const img::Rect b = d.selectionBounds();
                m_crop = {};
                m_crop.on = true; m_crop.viewId = v.id;
                m_crop.x0 = float(b.x0); m_crop.y0 = float(b.y0); m_crop.x1 = float(b.x1); m_crop.y1 = float(b.y1);
            }
        }
        snapStep();
        break;
    case Tool::Eyedropper:
        flowCheck(f, "Sample all layers", &m_sampleMerged);
        flowText(f, "Click: foreground.  Right click: background.");
        break;
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Clone:
        if (m_tool == Tool::Clone) {
            if (flowIcon(f, "##pick", Icon::Target, "Pick the source: then click the picture where to copy from.\n"
                                                    "(Alt+click does it directly.)", m_clonePick))
                m_clonePick = true;
            flowText(f, m_clonePick ? "Click the source..." : m_cloneHasSrc ? "Source set" : "No source yet");
            f.rule();
        }
        brushSize();
        flowPercent(f, "Opacity", "opa", m_brush.opacity, 5.0f, "Keys 1..0: 10 % .. 100 %");
        f.rule();
        steady();
        break;
    case Tool::Blur:
    case Tool::Dodge:
        if (m_tool == Tool::Blur) {
            if (flowIcon(f, "##blur", Icon::Blur, "Blur: softens  (Shift+R)", m_sharpen == 0)) m_sharpen = 0;
            if (flowIcon(f, "##sharp", Icon::Sharpen, "Sharpen: crisps  (Shift+R)", m_sharpen == 1)) m_sharpen = 1;
        } else {
            if (flowIcon(f, "##dodge", Icon::Dodge, "Dodge: lightens, mostly the mid-tones  (Shift+O)", m_tone == 0)) m_tone = 0;
            if (flowIcon(f, "##burn", Icon::Burn, "Burn: darkens, mostly the mid-tones  (Shift+O)", m_tone == 1)) m_tone = 1;
            if (flowIcon(f, "##sponge", Icon::Sponge, "Sponge: takes colour out  (Shift+O)", m_tone == 2)) m_tone = 2;
            if (flowIcon(f, "##sat", Icon::Saturate, "Sponge, saturate: puts colour in  (Shift+O)", m_tone == 3)) m_tone = 3;
        }
        f.rule();
        brushSize();
        flowPercent(f, m_tool == Tool::Dodge && m_tone < 2 ? "Exposure" : "Strength", "str", m_retouch, 5.0f,
                    "Keys 1..0: 10 % .. 100 %");
        f.rule();
        steady();
        break;
    case Tool::Fill:
        flowPercent(f, "Tolerance", "tol", m_tolerance, 2.0f);
        flowPercent(f, "Opacity", "fop", m_fillOpacity, 5.0f);
        flowCheck(f, "Contiguous", &m_contiguous);
        flowCheck(f, "Sample all layers", &m_sampleMerged);
        f.rule();
        if (flowButton(f, d.hasSelection() ? "Fill selection" : "Fill layer", "With the foreground colour  (Alt+Backspace)")) {
            d.fillSelection(m_fg);
            rememberColor(m_fg);
        }
        break;
    case Tool::Gradient:
        if (flowIcon(f, "##lin", Icon::Gradient, "Linear gradient", !m_radial)) m_radial = false;
        if (flowIcon(f, "##rad", Icon::RadialGradient, "Radial gradient", m_radial)) m_radial = true;
        flowCheck(f, "Reverse", &m_reverse, "Background colour at the start, foreground at the end.");
        f.rule();
        flowPercent(f, "Opacity", "gop", m_fillOpacity, 5.0f);
        snapStep();
        constrain("45 degree steps");
        break;
    case Tool::Shape: {
        static const Icon icons[] = {Icon::Line, Icon::RectOutline, Icon::RectFilled, Icon::EllipseOutline, Icon::EllipseFilled};
        static const char* tips[] = {"Line", "Rectangle", "Filled rectangle", "Ellipse", "Filled ellipse"};
        for (int i = 0; i < 5; ++i) {
            ImGui::PushID(i);
            if (flowIcon(f, "##shape", icons[i], tips[i], m_shape == i)) m_shape = i;
            ImGui::PopID();
        }
        f.rule();
        if (m_shape != 2 && m_shape != 4)
            flowStepper(f, "Width", "lw", m_lineWidth, m_lineWidth < 10 ? 1.0f : 5.0f, 1.0f, 400.0f, "%.0f px");
        flowPercent(f, "Opacity", "sop", m_brush.opacity, 5.0f);
        snapStep();
        constrain(m_shape == 0 ? "45 degree steps" : "Square / circle");
        break;
    }
    case Tool::Text: {
        f.next(em() * 9.0f);
        if (bigButton((m_fontName + "##font").c_str(), "Pick a font", em() * 9.0f)) {
            m_fontSearch[0] = '\0';
            ImGui::OpenPopup("##fonts");
        }
        ImGui::SetNextWindowSize(ImVec2(em() * 20.0f, em() * 26.0f));
        if (ImGui::BeginPopup("##fonts")) {
            ui::searchBox("##fs", m_fontSearch, sizeof m_fontSearch, "Search fonts...");
            ImGui::BeginChild("##fl");
            for (const auto& ff : img::systemFonts()) {
                if (!ui::icontains(ff.name.c_str(), m_fontSearch)) continue;
                if (ImGui::Selectable(ff.name.c_str(), ff.name == m_fontName, 0, ImVec2(0.0f, em() * 1.5f))) {
                    m_fontName = ff.name;
                    m_font = ff.path;
                    m_text.dirty = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        if (flowStepper(f, "Size", "tsize", m_textSize, m_textSize < 24 ? 2.0f : m_textSize < 100 ? 4.0f : 16.0f, 6.0f, 1000.0f, "%.0f px"))
            m_text.dirty = true;
        f.rule();
        f.next(em() * 16.0f);
        if (m_focusText) { ImGui::SetKeyboardFocusHere(); m_focusText = false; }
        if (ImGui::InputTextMultiline("##text", m_textBuf, sizeof m_textBuf, ImVec2(em() * 16.0f, ImGui::GetFrameHeight() * 1.6f)))
            m_text.dirty = true;
        ImGui::SetItemTooltip("%s", m_text.on ? "Type here. Click the picture again to move the text." : "Click the picture where the text goes.");
        if (m_text.on) {
            if (flowIcon(f, "##tok", Icon::Check, "Done: keep the text as a layer of its own")) textEnd(true);
            if (flowIcon(f, "##tno", Icon::Cancel, "Throw the text away  (Esc)")) textEnd(false);
        }
        snapStep();
        break;
    }
    case Tool::Hand:
    case Tool::Zoom:
        if (m_tool == Tool::Zoom) {
            if (flowIcon(f, "##zi", Icon::ZoomIn, "Zoom in")) zoomAt(v, nextZoom(v.zoom, +1), m_center);
            if (flowIcon(f, "##zo", Icon::ZoomOut, "Zoom out")) zoomAt(v, nextZoom(v.zoom, -1), m_center);
        }
        if (flowButton(f, "100 %", "Ctrl+1")) zoomAt(v, 1.0f, m_center);
        if (flowButton(f, "Fit screen", "Ctrl+0")) v.fitPending = true;
        break;
    default:
        break;
    }
    // A transform in progress: its numbers, whatever the tool.
    if (xfHere) {
        img::Xform& x = m_xf.xf;
        bool ch = false;
        ch |= flowStepper(f, "X", "xx", x.tx, 1.0f, -16384.0f, 16384.0f, "%+.0f", "Moved across, pixels", em() * 6.5f);
        ch |= flowStepper(f, "Y", "xy", x.ty, 1.0f, -16384.0f, 16384.0f, "%+.0f", "Moved down, pixels", em() * 6.5f);
        float wp = x.sx * 100.0f, hp = x.sy * 100.0f;
        if (flowStepper(f, "W", "xw", wp, 5.0f, -1000.0f, 1000.0f, "%.0f %%", "Width, percent", em() * 6.5f)) {
            if (m_xf.keepAspect && x.sx != 0.0f) x.sy *= (wp / 100.0f) / x.sx;
            x.sx = wp / 100.0f; ch = true;
        }
        if (flowIcon(f, "##chain", Icon::Chain, "Keep the proportions", m_xf.keepAspect)) m_xf.keepAspect = !m_xf.keepAspect;
        if (flowStepper(f, "H", "xh", hp, 5.0f, -1000.0f, 1000.0f, "%.0f %%", "Height, percent", em() * 6.5f)) {
            if (m_xf.keepAspect && x.sy != 0.0f) x.sx *= (hp / 100.0f) / x.sy;
            x.sy = hp / 100.0f; ch = true;
        }
        ch |= flowStepper(f, "Angle", "xa", x.rot, 15.0f, -180.0f, 180.0f, "%.0f deg", "Turned clockwise", em() * 6.5f);
        if (flowIcon(f, "##xfh", Icon::FlipH, "Flip horizontal")) { x.sx = -x.sx; ch = true; }
        if (flowIcon(f, "##xfv", Icon::FlipV, "Flip vertical")) { x.sy = -x.sy; ch = true; }
        if (flowIcon(f, "##xccw", Icon::RotateCCW, "Turn 90 degrees counter-clockwise")) { x.rot -= 90.0f; ch = true; }
        if (flowIcon(f, "##xcw", Icon::RotateCW, "Turn 90 degrees clockwise")) { x.rot += 90.0f; ch = true; }
        if (x.rot > 180.0f) x.rot -= 360.0f;
        if (x.rot < -180.0f) x.rot += 360.0f;
        f.rule();
        if (flowIcon(f, "##xok", Icon::Check, "Apply the transform  (Enter)")) transformCommit(v);
        else if (flowIcon(f, "##xno", Icon::Cancel, "Cancel the transform  (Esc)")) transformCancel(v);
        else if (ch) transformApply(v);
    }
    const float sp = ImGui::GetStyle().ItemSpacing.y, want = barY + barRow() + sp, now = ImGui::GetCursorPosY();
    if (now < want - 0.5f) ImGui::Dummy(ImVec2(1.0f, want - now - sp));
}

// --- The tool column ----------------------------------------------------------------------

void ImageEditor::toolBar() {
    const float side = std::round(em() * 2.1f);
    auto iconOf = [&](Tool t) {
        switch (t) {
        case Tool::Move: return Icon::Move;
        case Tool::Select: return m_ellipse ? Icon::MarqueeEllipse : Icon::Marquee;
        case Tool::Lasso: return m_polyLasso ? Icon::PolyLasso : Icon::Lasso;
        case Tool::Wand: return Icon::Wand;
        case Tool::Crop: return Icon::Crop;
        case Tool::Eyedropper: return Icon::Eyedropper;
        case Tool::Brush: return Icon::Brush;
        case Tool::Clone: return Icon::Clone;
        case Tool::Eraser: return Icon::Eraser;
        case Tool::Gradient: return m_radial ? Icon::RadialGradient : Icon::Gradient;
        case Tool::Fill: return Icon::Bucket;
        case Tool::Blur: return m_sharpen ? Icon::Sharpen : Icon::Blur;
        case Tool::Dodge: return m_tone == 0 ? Icon::Dodge : m_tone == 1 ? Icon::Burn : m_tone == 2 ? Icon::Sponge : Icon::Saturate;
        case Tool::Text: return Icon::Text;
        case Tool::Shape: {
            static const Icon s[] = {Icon::Line, Icon::RectOutline, Icon::RectFilled, Icon::EllipseOutline, Icon::EllipseFilled};
            return s[std::clamp(m_shape, 0, 4)];
        }
        case Tool::Hand: return Icon::Hand;
        default: return Icon::Zoom;
        }
    };
    // Which tools have kinds to pick from (the little corner mark, the right-click menu).
    struct Kind { Icon icon; const char* name; };
    auto kinds = [&](Tool t, std::vector<Kind>& out) {
        out.clear();
        if (t == Tool::Select) out = {{Icon::Marquee, "Rectangular marquee"}, {Icon::MarqueeEllipse, "Elliptical marquee"}};
        if (t == Tool::Lasso) out = {{Icon::Lasso, "Lasso"}, {Icon::PolyLasso, "Polygonal lasso"}};
        if (t == Tool::Blur) out = {{Icon::Blur, "Blur"}, {Icon::Sharpen, "Sharpen"}};
        if (t == Tool::Dodge) out = {{Icon::Dodge, "Dodge"}, {Icon::Burn, "Burn"}, {Icon::Sponge, "Sponge (desaturate)"},
                                     {Icon::Saturate, "Sponge (saturate)"}};
        if (t == Tool::Shape) out = {{Icon::Line, "Line"}, {Icon::RectOutline, "Rectangle"}, {Icon::RectFilled, "Filled rectangle"},
                                     {Icon::EllipseOutline, "Ellipse"}, {Icon::EllipseFilled, "Filled ellipse"}};
        if (t == Tool::Gradient) out = {{Icon::Gradient, "Linear gradient"}, {Icon::RadialGradient, "Radial gradient"}};
    };
    auto pickKind = [&](Tool t, int k) {
        if (t == Tool::Select) m_ellipse = k == 1;
        if (t == Tool::Lasso) { lassoCancel(); m_polyLasso = k == 1; }
        if (t == Tool::Blur) m_sharpen = k;
        if (t == Tool::Dodge) m_tone = k;
        if (t == Tool::Shape) m_shape = k;
        if (t == Tool::Gradient) m_radial = k == 1;
    };
    std::vector<Kind> ks;
    static int flyout = -1;
    for (int t = 0; t < int(Tool::Count); ++t) {
        const ToolDef& td = kTools[t];
        if (t % 2) ImGui::SameLine();
        ImGui::PushID(t);
        if (iconButton("##tool", iconOf(Tool(t)), nullptr, m_tool == Tool(t), side)) setTool(Tool(t));
        kinds(Tool(t), ks);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s  (%s)%s\n%s", td.name, td.key, ks.empty() ? "" : "  -- right click: its kinds", td.tip);
        if (!ks.empty()) {
            const ImVec2 b = ImGui::GetItemRectMax();
            const float k = side * 0.16f;
            ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(b.x - 3.0f, b.y - 3.0f - k), ImVec2(b.x - 3.0f, b.y - 3.0f),
                                                          ImVec2(b.x - 3.0f - k, b.y - 3.0f), ImGui::GetColorU32(ImGuiCol_Text));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { flyout = t; ImGui::OpenPopup("##kinds"); }
        }
        if (flyout == t && ImGui::BeginPopup("##kinds")) {
            for (int k = 0; k < int(ks.size()); ++k) {
                ImGui::PushID(k);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float s = ImGui::GetFrameHeight();
                if (ImGui::Selectable("##k", false, 0, ImVec2(em() * 13.0f, s))) { setTool(Tool(t)); pickKind(Tool(t), k); }
                drawIcon(ImGui::GetWindowDrawList(), ks[std::size_t(k)].icon, ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.7f,
                         ImGui::GetColorU32(ImGuiCol_Text));
                ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + s * 1.3f, p.y + (s - em()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                                                    ks[std::size_t(k)].name);
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// Foreground and background, the way Photoshop shows them: two overlapping
// squares, the swap arrow above, the black-and-white reset below.
void ImageEditor::fgBg() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const float w = ImGui::GetContentRegionAvail().x;
    const float sq = std::floor(w * 0.58f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float small = std::floor(w - sq) * 0.95f;
    // Background first, so the foreground lies on top of it.
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - sq, p.y + w - sq));
    if (ImGui::ColorButton("Background##bg", toIm(m_bg), ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip,
                           ImVec2(sq, sq)))
        ImGui::OpenPopup("##bgpop");
    ImGui::SetItemTooltip("Background colour %s -- click to change", img::hexOf(m_bg).c_str());
    ImGui::SetCursorScreenPos(p);
    if (ImGui::ColorButton("Foreground##fg", toIm(m_fg), ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip,
                           ImVec2(sq, sq)))
        ImGui::OpenPopup("##fgpop");
    ImGui::SetItemTooltip("Foreground colour %s -- click to change", img::hexOf(m_fg).c_str());
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + sq, p.y + sq), IM_COL32(0, 0, 0, 200), 0.0f, 0, 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - small, p.y));
    if (iconButton("##swap", Icon::Swap, "Swap foreground and background  (X)", false, small)) std::swap(m_fg, m_bg);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + w - small));
    if (iconButton("##defaults", Icon::Defaults, "Black and white  (D)", false, small)) { m_fg = {0, 0, 0, 1}; m_bg = {1, 1, 1, 1}; }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + w));
    ImGui::Dummy(ImVec2(w, 2.0f));
    colorPopup("##fgpop", m_fg, "Foreground colour");
    colorPopup("##bgpop", m_bg, "Background colour");
}

void ImageEditor::colorPopup(const char* id, img::Color& c, const char* title) {
    if (!ImGui::BeginPopup(id)) return;
    ui::title("%s", title);
    ImGui::SetNextItemWidth(em() * 17.0f);
    ImGui::ColorPicker4("##picker", &c.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueBar |
                                              ImGuiColorEditFlags_AlphaPreviewHalf);
    char hex[16];
    std::snprintf(hex, sizeof hex, "%s", img::hexOf(c).c_str() + 1);
    ImGui::SetNextItemWidth(em() * 6.0f);
    if (ImGui::InputText("Hex", hex, sizeof hex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        unsigned v = 0;
        if (std::sscanf(hex, "%x", &v) == 1 && std::strlen(hex) == 6) {
            c.r = ((v >> 16) & 255) / 255.0f; c.g = ((v >> 8) & 255) / 255.0f; c.b = (v & 255) / 255.0f;
        }
    }
    float a = c.a * 100.0f;
    if (ui::stepper("alpha", a, 5.0f, 0.0f, 100.0f, "%.0f %% opaque", em() * 12.0f)) c.a = a / 100.0f;
    if (bigButton("OK", nullptr, -1.0f)) { rememberColor(c); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

// --- The panels on the right ----------------------------------------------------------------

void ImageEditor::colorPanel() {
    const float w = std::min(ImGui::GetContentRegionAvail().x, em() * 11.0f);
    ImGui::SetNextItemWidth(w);
    ImGui::ColorPicker4("##fgpanel", &m_fg.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueBar |
                                                  ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                                                  ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_DisplayRGB |
                                                  ImGuiColorEditFlags_DisplayHex);
}

void ImageEditor::swatchesPanel() {
    // A click takes the colour as foreground, a right click as background.
    const int perRow = 10;
    const float cs = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 0.5f * (perRow - 1)) / perRow);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x * 0.5f, ImGui::GetStyle().ItemSpacing.x * 0.5f));
    auto swatch = [&](const img::Color& c, int i, int n) {
        if (n % perRow) ImGui::SameLine();
        ImGui::PushID(i);
        if (ImGui::ColorButton("##sw", toIm(c), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(cs, cs)))
            m_fg = c;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m_bg = c;
        ImGui::SetItemTooltip("%s -- click: foreground, right click: background", img::hexOf(c).c_str());
        ImGui::PopID();
    };
    const int n = int(sizeof kPalette / sizeof kPalette[0]);
    for (int i = 0; i < n; ++i) swatch(kPalette[i], i, i);
    // Greys between black and white fill the last row.
    for (int i = 0; i < 5; ++i) {
        const float g = (i + 1) / 6.0f;
        swatch(img::Color{g, g, g, 1}, 50 + i, n + i);
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ui::hint(m_recent.empty() ? "Colours you paint with collect here." : "Used lately");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x * 0.5f, ImGui::GetStyle().ItemSpacing.x * 0.5f));
    for (int i = 0; i < int(m_recent.size()); ++i) swatch(m_recent[std::size_t(i)], 100 + i, i);
    ImGui::PopStyleVar();
}

void ImageEditor::updateThumbs(View& v) {
    img::Document& d = v.doc;
    const int L = d.layerCount();
    const int T = 48;
    const int tw = d.width() >= d.height() ? T : std::max(1, T * d.width() / d.height());
    const int th = d.height() >= d.width() ? T : std::max(1, T * d.height() / d.width());
    const bool resized = tw != v.thumbW || th != v.thumbH;
    if (!resized && int(v.thumbs.size()) == L && v.thumbVer == d.contentVersion()) return;
    // While a stroke is going on, a few times a second is plenty.
    const double now = ImGui::GetTime();
    if (!resized && int(v.thumbs.size()) == L && (m_painting || m_moving || m_xf.drag >= 0) && now - v.thumbTime < 0.25) return;
    while (int(v.thumbs.size()) > L) { m_retired.push_back(v.thumbs.back()); v.thumbs.pop_back(); }
    // New layers get their texture below, made at the thumbnail's size.
    v.thumbs.resize(std::size_t(L));
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    std::vector<std::uint8_t> buf(std::size_t(tw) * th * 4);
    const int W = d.width(), H = d.height();
    for (int i = 0; i < L; ++i) {
        const img::Pixels& p = d.pixels(i);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                // Four samples a thumbnail pixel, averaged on premultiplied colour.
                float acc[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    const int sx = std::min(W - 1, int((x + 0.25f + 0.5f * (k & 1)) * W / tw));
                    const int sy = std::min(H - 1, int((y + 0.25f + 0.5f * (k >> 1)) * H / th));
                    const std::uint8_t* q = &p[(std::size_t(sy) * W + sx) * 4];
                    const float a = q[3] / 255.0f;
                    acc[0] += q[0] * a; acc[1] += q[1] * a; acc[2] += q[2] * a; acc[3] += a;
                }
                std::uint8_t* o = &buf[(std::size_t(y) * tw + x) * 4];
                for (int c = 0; c < 3; ++c) o[c] = std::uint8_t(acc[3] > 0 ? std::min(255.0f, acc[c] / acc[3] + 0.5f) : 0);
                o[3] = std::uint8_t(acc[3] / 4.0f * 255.0f + 0.5f);
            }
        fitzel::rhi::TextureHandle& t = v.thumbs[std::size_t(i)];
        // A texture keeps the size it was made with: a thumbnail of another
        // shape is a new one (the old one is freed once this frame is drawn).
        if (t && (gpu.textureDesc(t).width != tw || gpu.textureDesc(t).height != th)) {
            m_retired.push_back(t);
            t = {};
        }
        if (!t) {
            fitzel::rhi::TextureDesc td;
            td.format = fitzel::rhi::Format::RGBA8;
            td.width = tw;
            td.height = th;
            td.sampler = fitzel::rhi::SamplerDesc::linearClamp();
            t = gpu.createTexture(td, buf.data(), fitzel::rhi::Format::RGBA8);
        } else {
            gpu.updateTexture(t, fitzel::rhi::TextureRegion{}, fitzel::rhi::Format::RGBA8, buf.data());
        }
    }
    v.thumbW = tw;
    v.thumbH = th;
    v.thumbVer = d.contentVersion();
    v.thumbTime = now;
}

void ImageEditor::layersPanel(View& v) {
    img::Document& d = v.doc;
    updateThumbs(v);
    const int a = d.active();
    // The active layer's blend mode and opacity on top, as in Photoshop.
    ImGui::SetNextItemWidth(em() * 7.5f);
    if (ImGui::BeginCombo("##blend", img::blendName(d.layer(a).blend), ImGuiComboFlags_HeightLargest)) {
        for (int b = 0; b < int(img::Blend::Count); ++b)
            if (ImGui::Selectable(img::blendName(img::Blend(b)), int(d.layer(a).blend) == b, 0, ImVec2(0.0f, em() * 1.5f)))
                d.setBlend(a, img::Blend(b));
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Blend mode: how this layer mixes with the ones below");
    ImGui::SameLine();
    float op = d.layer(a).opacity * 100.0f;
    if (ui::stepper("lop", op, 5.0f, 0.0f, 100.0f, "%.0f %%", ImGui::GetContentRegionAvail().x)) d.setOpacity(a, op / 100.0f);
    ImGui::SetItemTooltip("Opacity of the layer");
    ImGui::AlignTextToFramePadding();
    ui::hint("Lock:");
    ImGui::SameLine();
    const bool locked = d.layer(a).lockAlpha;
    if (iconButton("##lockalpha", locked ? Icon::Lock : Icon::Unlock,
                   "Lock transparent pixels  (/): paint only where the layer already has paint", locked))
        d.setLockAlpha(a, !locked);

    const float rowH = std::round(em() * 2.4f);
    const float bottomH = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    ImGui::BeginChild("##layers", ImVec2(0.0f, std::max(rowH * 2.0f, ImGui::GetContentRegionAvail().y - bottomH)),
                      ImGuiChildFlags_Borders);
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    for (int i = d.layerCount() - 1; i >= 0; --i) {
        const img::Layer& l = d.layer(i);
        ImGui::PushID(i);
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::Selectable("##row", i == a, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(rowW, rowH))) {
            selectLayer(v, i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { m_renaming = i; m_renameFocus = true; }
        }
        // Drag a row onto another to restack (the arrow buttons below do it too).
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("IMGLAYER", &i, sizeof i);
            ImGui::Text("%s", l.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IMGLAYER")) {
                const int from = *static_cast<const int*>(pl->Data);
                d.moveLayerTo(from, i);
            }
            ImGui::EndDragDropTarget();
        }
        const float eye = rowH * 0.72f;
        ImGui::SetCursorScreenPos(ImVec2(row.x + 2.0f, row.y + (rowH - eye) * 0.5f));
        if (iconButton("##eye", l.visible ? Icon::Eye : Icon::EyeOff,
                       "Show or hide the layer. Alt+click: show only this one.", false, eye)) {
            if (ImGui::GetIO().KeyAlt) {
                bool others = false;
                for (int k = 0; k < d.layerCount(); ++k) others |= k != i && d.layer(k).visible;
                for (int k = 0; k < d.layerCount(); ++k)
                    if (k != i && d.layer(k).visible == others) d.setVisible(k, !others);
                if (!l.visible) d.setVisible(i, true);
            } else {
                d.setVisible(i, !l.visible);
            }
        }
        // The thumbnail, on the checks that mean "see-through".
        const float ts = rowH - 6.0f;
        const ImVec2 t0(row.x + eye + 8.0f, row.y + 3.0f);
        ImGui::SetCursorScreenPos(t0);
        ImGui::InvisibleButton("##thumb", ImVec2(ts, ts));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (ImGui::GetIO().KeyCtrl) d.selectLayerAlpha(img::SelOp::Replace, i);
            else selectLayer(v, i);
        }
        ImGui::SetItemTooltip("Ctrl+click: select what this layer has painted");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (i < int(v.thumbs.size()) && v.thumbW > 0) {
            const float k = ts / float(std::max(v.thumbW, v.thumbH));
            const float iw = v.thumbW * k, ih = v.thumbH * k;
            const ImVec2 q0(t0.x + (ts - iw) * 0.5f, t0.y + (ts - ih) * 0.5f), q1(q0.x + iw, q0.y + ih);
            textureSampling(dl, true);
            dl->AddImage(fitzel::imTexture(m_checker), q0, q1, ImVec2(0, 0), ImVec2(iw / 8.0f, ih / 8.0f));
            dl->AddImage(fitzel::imTexture(v.thumbs[std::size_t(i)]), q0, q1);
            textureSampling(dl, false);
            dl->AddRect(q0, q1, i == a ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : IM_COL32(0, 0, 0, 160));
        }
        // The name (or the field it is being renamed in), and what is unusual about the layer.
        const float tx = t0.x + ts + 8.0f;
        if (m_renaming == i) {
            ImGui::SetCursorScreenPos(ImVec2(tx, row.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::SetNextItemWidth(row.x + rowW - tx - 4.0f);
            if (m_renameFocus) {
                std::snprintf(m_layerName, sizeof m_layerName, "%s", l.name.c_str());
                ImGui::SetKeyboardFocusHere();
                m_renameFocus = false;
            }
            ImGui::InputText("##rename", m_layerName, sizeof m_layerName, ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemDeactivated()) {
                if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && m_layerName[0]) d.rename(i, m_layerName);
                m_renaming = -1;
            }
        } else {
            const ImU32 col = l.visible ? ink : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            char extra[64] = "";
            if (l.opacity < 1.0f || l.blend != img::Blend::Normal)
                std::snprintf(extra, sizeof extra, "%s%s%.0f %%", l.blend != img::Blend::Normal ? img::blendName(l.blend) : "",
                              l.blend != img::Blend::Normal ? "  " : "", l.opacity * 100.0f);
            const float ty = extra[0] ? row.y + rowH * 0.5f - em() * 0.95f : row.y + (rowH - em()) * 0.5f;
            dl->PushClipRect(ImVec2(tx, row.y), ImVec2(row.x + rowW - (l.lockAlpha ? rowH * 0.6f : 0.0f), row.y + rowH), true);
            dl->AddText(ImVec2(tx, ty), col, l.name.c_str());
            if (extra[0]) dl->AddText(ImGui::GetFont(), em() * 0.85f, ImVec2(tx, ty + em() * 1.05f), ImGui::GetColorU32(ImGuiCol_TextDisabled), extra);
            dl->PopClipRect();
            if (l.lockAlpha)
                drawIcon(dl, Icon::Lock, ImVec2(row.x + rowW - rowH * 0.35f, row.y + rowH * 0.5f), rowH * 0.38f,
                         ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rowH + ImGui::GetStyle().ItemSpacing.y * 0.5f));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        ImGui::PopID();
    }
    ImGui::EndChild();
    // The row of icons under the list.
    if (iconButton("##lnew", Icon::LayerNew, "New layer  (Shift+Ctrl+N)")) d.addLayer();
    ImGui::SameLine();
    if (iconButton("##ldup", Icon::Duplicate, "Duplicate the layer -- or with a selection: layer via copy  (Ctrl+J)")) {
        cancelGesture(v);
        if (d.hasSelection()) d.layerViaCopy(false); else d.duplicateLayer();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(a >= d.layerCount() - 1);
    if (iconButton("##lup", Icon::Up, "Bring forward  (Ctrl+])")) d.moveLayerTo(a, a + 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(a <= 0);
    if (iconButton("##ldown", Icon::Down, "Send backward  (Ctrl+[)")) d.moveLayerTo(a, a - 1);
    ImGui::SameLine();
    if (iconButton("##lmerge", Icon::Merge, "Merge down  (Ctrl+E)")) d.mergeDown();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(d.layerCount() < 2);
    if (iconButton("##ldel", Icon::Trash, "Delete the layer")) d.deleteLayer();
    ImGui::EndDisabled();
}

void ImageEditor::historyPanel(View& v) {
    img::Document& d = v.doc;
    int applied = 0;
    const std::vector<std::string> h = d.history(applied);
    ImGui::BeginChild("##hist", ImVec2(0.0f, std::max(em() * 6.0f, ImGui::GetContentRegionAvail().y - em() * 2.6f)),
                      ImGuiChildFlags_Borders);
    const float rowH = em() * 1.5f;
    if (ImGui::Selectable("Opened", applied == 0, 0, ImVec2(0.0f, rowH))) { cancelGesture(v); d.jumpTo(0); }
    for (int i = 0; i < int(h.size()); ++i) {
        ImGui::PushID(i);
        const bool undone = i >= applied;
        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(h[std::size_t(i)].c_str(), applied == i + 1, 0, ImVec2(0.0f, rowH))) {
            cancelGesture(v);
            if (m_xf.on) transformCancel(v);
            d.jumpTo(i + 1);
        }
        if (undone) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (ImGui::GetScrollMaxY() > 0.0f && applied == int(h.size())) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ui::hint("Click a step to go back to it; the later ones\nstay until you do something new.");
}

// --- Dialogs --------------------------------------------------------------------------------

void ImageEditor::dialog(View* v) {
    if (m_pane == Pane::None) return;
    if (!v && m_pane != Pane::New) { m_pane = Pane::None; return; }
    static const char* titles[] = {"", "Filter", "Curves", "Image size", "Canvas size", "New picture", "Modify selection",
                                   "Fill", "Stroke"};
    std::string title = titles[int(m_pane)];
    if (m_pane == Pane::Filter) title = img::filterInfo(m_filter).name;
    if (m_pane == Pane::Modify) {
        static const char* mods[] = {"Expand selection", "Contract selection", "Feather selection", "Border selection", "Smooth selection"};
        title = mods[int(m_modOp)];
    }
    title += "###imgdlg";
    if (!m_paneOpened) { ImGui::OpenPopup("###imgdlg"); m_paneOpened = true; }
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // The picture behind stays as bright as it is: the preview is the point.
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
    bool open = true;
    const bool shown = ImGui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    if (!shown) {
        // Closed by its x (or never shown): that is a cancel.
        if (v) closePane(*v, false); else m_pane = Pane::None;
        return;
    }
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const float bw = em() * 7.0f;
    auto okCancel = [&](bool& ok, bool& cancel) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ok = bigButton("OK", "Enter", bw) || enter;
        ImGui::SameLine();
        cancel = bigButton("Cancel", "Esc", bw) || esc;
    };
    bool ok = false, cancel = false;
    switch (m_pane) {
    case Pane::Filter:
        filterDialog(*v);
        okCancel(ok, cancel);
        if (ok) closePane(*v, true); else if (cancel) closePane(*v, false);
        break;
    case Pane::Curves:
        curvesDialog(*v);
        okCancel(ok, cancel);
        if (ok) closePane(*v, true); else if (cancel) closePane(*v, false);
        break;
    case Pane::Resize:
    case Pane::Canvas: {
        img::Document& d = v->doc;
        const bool resize = m_pane == Pane::Resize;
        ui::hint(resize ? "Scales every layer." : "Adds or cuts away room around the picture; nothing is scaled.");
        const int ow = d.width(), oh = d.height();
        int nw = m_resW, nh = m_resH;
        ImGui::SetNextItemWidth(em() * 9.0f);
        if (ImGui::InputInt("Width", &nw, 1, 64) && m_keepAspect && resize) nh = std::max(1, int(std::lround(double(nw) * oh / ow)));
        ImGui::SetNextItemWidth(em() * 9.0f);
        if (ImGui::InputInt("Height", &nh, 1, 64) && m_keepAspect && resize) nw = std::max(1, int(std::lround(double(nh) * ow / oh)));
        m_resW = std::clamp(nw, 1, 16384);
        m_resH = std::clamp(nh, 1, 16384);
        if (resize) {
            ImGui::Checkbox("Keep proportions", &m_keepAspect);
            const struct { const char* l; double k; } ks[] = {{"25 %", 0.25}, {"50 %", 0.5}, {"200 %", 2.0}, {"400 %", 4.0}};
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine();
                if (bigButton(ks[i].l, nullptr, em() * 4.5f)) {
                    m_resW = std::max(1, int(std::lround(ow * ks[i].k)));
                    m_resH = std::max(1, int(std::lround(oh * ks[i].k)));
                }
            }
            if (bigButton("Power of two", "The nearest power of two on each side -- what GPUs like for textures.", -1.0f)) {
                auto p2 = [](int x) { int p = 1; while (p * 2 <= x) p *= 2; return (x - p) > (p * 2 - x) ? p * 2 : p; };
                m_resW = p2(ow);
                m_resH = p2(oh);
            }
        } else {
            ImGui::TextUnformatted("Anchor");
            const float s = ImGui::GetFrameHeight() * 1.2f;
            static const Icon arrows[3][3] = {{Icon::Up, Icon::Up, Icon::Up}, {Icon::Left, Icon::SelReplace, Icon::Right},
                                              {Icon::Down, Icon::Down, Icon::Down}};
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x) {
                    if (x) ImGui::SameLine();
                    ImGui::PushID(y * 3 + x);
                    const bool on = m_ancX == x && m_ancY == y;
                    if (iconButton("##anc", on ? Icon::SelReplace : arrows[y][x], "Keep the picture at this side", on, s)) {
                        m_ancX = x; m_ancY = y;
                    }
                    ImGui::PopID();
                }
        }
        ImGui::Text("%d x %d  ->  %d x %d", ow, oh, m_resW, m_resH);
        okCancel(ok, cancel);
        if (ok) {
            if (resize) d.resize(m_resW, m_resH); else d.canvasSize(m_resW, m_resH, m_ancX, m_ancY);
            v->fitPending = true;
            m_pane = Pane::None;
        } else if (cancel) {
            m_pane = Pane::None;
        }
        break;
    }
    case Pane::New:
        if (newControls(m_newW, m_newH, m_newFill, m_fg, [&](int w, int h, img::Color c) { newImage(w, h, c); }) || esc ||
            bigButton("Cancel", nullptr, -1.0f))
            m_pane = Pane::None;
        break;
    case Pane::Modify: {
        img::Document& d = v->doc;
        static const char* what[] = {"Grow it by", "Shrink it by", "Soften the edge over", "Keep a band this wide", "Round it off over"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(what[int(m_modOp)]);
        ImGui::SameLine();
        ui::stepper("modr", m_modR, m_modR < 10 ? 1.0f : 5.0f, 1.0f, 500.0f, "%.0f px", em() * 9.0f);
        okCancel(ok, cancel);
        if (ok) { d.modifySelection(m_modOp, m_modR); m_pane = Pane::None; }
        else if (cancel) m_pane = Pane::None;
        break;
    }
    case Pane::Fill: {
        img::Document& d = v->doc;
        static const char* names[] = {"Foreground", "Background", "Black", "50 % grey", "White"};
        ImGui::TextUnformatted("Contents");
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine();
            if (toggle(names[i], m_fillWhat == i)) m_fillWhat = i;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Opacity");
        ImGui::SameLine();
        float o = m_fillDlgOpacity * 100.0f;
        if (ui::stepper("fdo", o, 5.0f, 0.0f, 100.0f, "%.0f %%", em() * 9.0f)) m_fillDlgOpacity = o / 100.0f;
        ui::hint(d.hasSelection() ? "Fills the selection." : "Fills the whole layer.");
        okCancel(ok, cancel);
        if (ok) {
            img::Color c = m_fillWhat == 0 ? m_fg : m_fillWhat == 1 ? m_bg : m_fillWhat == 2 ? img::Color{0, 0, 0, 1}
                         : m_fillWhat == 3 ? img::Color{0.5f, 0.5f, 0.5f, 1} : img::Color{1, 1, 1, 1};
            c.a *= m_fillDlgOpacity;
            d.fillSelection(c);
            m_pane = Pane::None;
        } else if (cancel) {
            m_pane = Pane::None;
        }
        break;
    }
    case Pane::Stroke: {
        img::Document& d = v->doc;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Width");
        ImGui::SameLine();
        ui::stepper("sw", m_strokeW, m_strokeW < 10 ? 1.0f : 5.0f, 1.0f, 250.0f, "%.0f px", em() * 9.0f);
        ImGui::TextUnformatted("Location");
        static const char* where[] = {"Inside", "Center", "Outside"};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            if (toggle(where[i], m_strokeWhere == i, em() * 5.5f)) m_strokeWhere = i;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Colour");
        ImGui::SameLine();
        ImGui::ColorButton("##sc", toIm(m_fg), ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(em() * 3.0f, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ui::hint("the foreground colour");
        okCancel(ok, cancel);
        if (ok) { d.strokeSelection(m_fg, m_strokeW, m_strokeWhere); rememberColor(m_fg); m_pane = Pane::None; }
        else if (cancel) m_pane = Pane::None;
        break;
    }
    default:
        m_pane = Pane::None;
        break;
    }
    if (m_pane == Pane::None) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ImageEditor::filterDialog(View& v) {
    img::Document& d = v.doc;
    if (!d.editing()) { m_pane = Pane::None; return; }   // something else took the edit
    const img::FilterInfo& fi = img::filterInfo(m_filter);
    ImGui::PushTextWrapPos(em() * 24.0f);
    ui::hint("%s", fi.tip);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    bool changed = false;
    for (int k = 0; k < fi.params; ++k) {
        ImGui::PushID(k);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(fi.label[k]);
        ImGui::SameLine(em() * 8.5f);
        changed |= ui::stepper("p", m_fp[k], fi.step[k], fi.lo[k], fi.hi[k], fi.fmt[k], em() * 14.0f);
        ImGui::PopID();
    }
    if ((m_filter == img::Filter::Noise || m_filter == img::Filter::Clouds) &&
        bigButton(m_filter == img::Filter::Noise ? "New grain" : "New clouds", nullptr, -1.0f)) {
        ++m_seed;
        changed = true;
    }
    if (bigButton("Defaults", nullptr, -1.0f)) {
        for (int k = 0; k < 3; ++k) m_fp[k] = fi.def[k];
        changed = true;
    }
    if (ImGui::Checkbox("Preview", &m_preview)) {
        if (m_preview) changed = true;
        else d.restoreFromBase({0, 0, d.width(), d.height()});
    }
    if (d.hasSelection()) { ImGui::SameLine(); ui::hint("Only inside the selection."); }
    if (changed && m_preview) {
        const img::Color pair[2] = {m_fg, m_bg};
        d.filter(m_filter, m_fp, m_seed, pair);
    }
}

void ImageEditor::curvesDialog(View& v) {
    img::Document& d = v.doc;
    if (!d.editing()) { m_pane = Pane::None; return; }
    static const char* chans[] = {"RGB", "Red", "Green", "Blue"};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Channel");
    for (int c = 0; c < 4; ++c) {
        ImGui::SameLine();
        if (toggle(chans[c], m_curveCh == c, em() * 4.0f)) { m_curveCh = c; m_curveSel = -1; }
    }
    auto& pts = m_curve[m_curveCh];
    bool changed = false;
    // The graph: input across, output up, the histogram behind.
    const float G = std::round(em() * 16.0f);
    const ImVec2 g0 = ImGui::GetCursorScreenPos(), g1(g0.x + G, g0.y + G);
    ImGui::InvisibleButton("##curve", ImVec2(G, G), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(g0, g1, IM_COL32(24, 25, 28, 255));
    const int hc = m_curveCh;
    std::uint32_t mx = 1;
    for (int k = 1; k < 255; ++k) mx = std::max(mx, m_hist[hc][k]);
    static const ImU32 histCol[4] = {IM_COL32(150, 150, 150, 90), IM_COL32(230, 80, 80, 90), IM_COL32(80, 210, 90, 90),
                                     IM_COL32(90, 130, 240, 90)};
    for (int k = 0; k < 256; ++k) {
        const float hgt = std::min(1.0f, float(m_hist[hc][k]) / float(mx)) * G;
        const float x = g0.x + (k + 0.5f) / 256.0f * G;
        dl->AddLine(ImVec2(x, g1.y), ImVec2(x, g1.y - hgt), histCol[hc], G / 256.0f + 0.5f);
    }
    for (int k = 1; k < 4; ++k) {
        dl->AddLine(ImVec2(g0.x + G * k / 4, g0.y), ImVec2(g0.x + G * k / 4, g1.y), IM_COL32(255, 255, 255, 28));
        dl->AddLine(ImVec2(g0.x, g0.y + G * k / 4), ImVec2(g1.x, g0.y + G * k / 4), IM_COL32(255, 255, 255, 28));
    }
    dl->AddLine(ImVec2(g0.x, g1.y), ImVec2(g1.x, g0.y), IM_COL32(255, 255, 255, 40));
    dl->AddRect(g0, g1, IM_COL32(255, 255, 255, 60));
    auto toScreenPt = [&](float x, float y) { return ImVec2(g0.x + x / 255.0f * G, g1.y - y / 255.0f * G); };
    std::uint8_t lut[256];
    img::curveTable(pts, lut);
    static const ImU32 curveCol[4] = {IM_COL32(235, 235, 235, 255), IM_COL32(240, 90, 90, 255), IM_COL32(90, 220, 100, 255),
                                      IM_COL32(100, 150, 255, 255)};
    ImVec2 poly[256];
    for (int k = 0; k < 256; ++k) poly[k] = toScreenPt(float(k), float(lut[k]));
    dl->PushClipRect(g0, g1, true);
    dl->AddPolyline(poly, 256, curveCol[m_curveCh], 0, 2.0f);
    dl->PopClipRect();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mxv = std::clamp((mouse.x - g0.x) / G * 255.0f, 0.0f, 255.0f);
    const float myv = std::clamp((g1.y - mouse.y) / G * 255.0f, 0.0f, 255.0f);
    int nearPt = -1;
    for (int i = 0; i < int(pts.size()); ++i) {
        const ImVec2 s = toScreenPt(pts[std::size_t(i)].first, pts[std::size_t(i)].second);
        if (std::fabs(s.x - mouse.x) < 8.0f && std::fabs(s.y - mouse.y) < 8.0f) nearPt = i;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (nearPt >= 0) { m_curveSel = nearPt; m_curveDrag = nearPt; }
        else {
            // A new point on the curve under the pointer.
            std::size_t at = 0;
            while (at < pts.size() && pts[at].first < mxv) ++at;
            if ((at == 0 || pts[at - 1].first < mxv - 1.0f) && (at == pts.size() || pts[at].first > mxv + 1.0f)) {
                pts.insert(pts.begin() + std::ptrdiff_t(at), {std::round(mxv), std::round(myv)});
                m_curveSel = m_curveDrag = int(at);
                changed = true;
            }
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && nearPt > 0 && nearPt < int(pts.size()) - 1) {
        pts.erase(pts.begin() + nearPt);
        m_curveSel = -1;
        changed = true;
    }
    if (m_curveDrag >= 0 && m_curveDrag < int(pts.size())) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const int i = m_curveDrag;
            const float lo = i > 0 ? pts[std::size_t(i - 1)].first + 1.0f : 0.0f;
            const float hi = i + 1 < int(pts.size()) ? pts[std::size_t(i + 1)].first - 1.0f : 255.0f;
            const float nx = std::round(std::clamp(mxv, lo, hi)), ny = std::round(myv);
            if (nx != pts[std::size_t(i)].first || ny != pts[std::size_t(i)].second) {
                pts[std::size_t(i)] = {nx, ny};
                changed = true;
            }
        } else {
            m_curveDrag = -1;
        }
    }
    for (int i = 0; i < int(pts.size()); ++i) {
        const ImVec2 s = toScreenPt(pts[std::size_t(i)].first, pts[std::size_t(i)].second);
        if (i == m_curveSel) dl->AddRectFilled(ImVec2(s.x - 4, s.y - 4), ImVec2(s.x + 4, s.y + 4), curveCol[m_curveCh]);
        else dl->AddRect(ImVec2(s.x - 4, s.y - 4), ImVec2(s.x + 4, s.y + 4), curveCol[m_curveCh], 0.0f, 0, 1.5f);
    }
    ImGui::SetItemTooltip("Click the curve to add a point and drag it; right click a point to remove it.");
    // The picked point in numbers: steppers, no aiming needed.
    if (m_curveSel >= 0 && m_curveSel < int(pts.size())) {
        auto& p = pts[std::size_t(m_curveSel)];
        const float lo = m_curveSel > 0 ? pts[std::size_t(m_curveSel - 1)].first + 1.0f : 0.0f;
        const float hi = m_curveSel + 1 < int(pts.size()) ? pts[std::size_t(m_curveSel + 1)].first - 1.0f : 255.0f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Input");
        ImGui::SameLine(em() * 4.5f);
        changed |= ui::stepper("cin", p.first, 1.0f, lo, hi, "%.0f", em() * 11.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Output");
        ImGui::SameLine(em() * 4.5f);
        changed |= ui::stepper("cout", p.second, 1.0f, 0.0f, 255.0f, "%.0f", em() * 11.5f);
    } else {
        ui::hint("Click the curve to add a point.");
    }
    const float qw = std::floor((G - ImGui::GetStyle().ItemSpacing.x) / 2.0f);
    if (ImGui::Button("Reset", ImVec2(qw, 0))) { pts = {{0, 0}, {255, 255}}; m_curveSel = -1; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("More contrast", ImVec2(qw, 0))) { pts = {{0, 0}, {64, 48}, {192, 208}, {255, 255}}; m_curveSel = -1; changed = true; }
    ImGui::SetItemTooltip("A gentle S: darker shadows, brighter highlights.");
    if (ImGui::Button("Lighter", ImVec2(qw, 0))) { pts = {{0, 0}, {128, 160}, {255, 255}}; m_curveSel = -1; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Darker", ImVec2(qw, 0))) { pts = {{0, 0}, {128, 96}, {255, 255}}; m_curveSel = -1; changed = true; }
    if (ImGui::Checkbox("Preview", &m_preview)) {
        if (m_preview) changed = true;
        else d.restoreFromBase({0, 0, d.width(), d.height()});
    }
    if (changed && m_preview) applyCurvesPreview(v);
}

// --- The status line ------------------------------------------------------------------------

const char* ImageEditor::toolHint() const {
    if (m_xf.on) return "Drag inside: move. Handles: scale (Shift keeps the shape). Outside a corner: turn. Enter applies.";
    if (m_crop.on) return "Drag the handles or inside the box; Enter crops, Esc cancels.";
    if (m_lasso.on && m_polyLasso) return "Click the next corner; the first one (or a double-click, or Enter) closes it.";
    switch (m_tool) {
    case Tool::Brush: return "Shift+click: a line from the last stroke.  Right / Alt+click: pick a colour.";
    case Tool::Clone: return m_clonePick ? "Click (or Alt+click) where to copy from." : "Paint where the copy goes.  Alt+click: a new source.";
    case Tool::Select: return "Shift: add  Alt: subtract.  Drag inside the selection to move it.";
    case Tool::Lasso: return m_polyLasso ? "Click corner after corner." : "Drag round what you want.";
    case Tool::Move: return "Drag, or the arrow keys.  Ctrl+T: free transform.";
    case Tool::Zoom: return "Click: zoom in.  Alt+click / right click: zoom out.";
    case Tool::Fill:
    case Tool::Gradient:
    case Tool::Shape: return "Right / Alt+click: pick a colour.";
    default: return "";
    }
}

void ImageEditor::statusLine(View& v) {
    const img::Document& d = v.doc;
    char buf[256];
    int n = std::snprintf(buf, sizeof buf, "%d x %d px   %d layer%s   %.0f %%", d.width(), d.height(), d.layerCount(),
                          d.layerCount() == 1 ? "" : "s", v.zoom * 100.0f);
    if (d.hasSelection() && n > 0 && n < int(sizeof buf)) {
        const img::Rect b = d.selectionBounds();
        n += std::snprintf(buf + n, sizeof buf - std::size_t(n), "   selection %d x %d at %d, %d", b.w(), b.h(), b.x0, b.y0);
    }
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(buf);
    if (!m_hoverInfo.empty()) {
        ImGui::SameLine(0.0f, em() * 2.0f);
        ImGui::TextUnformatted(m_hoverInfo.c_str());
    }
    const char* hint = toolHint();
    if (hint[0]) {
        const float w = ImGui::CalcTextSize(hint).x * 0.9f;
        ImGui::SameLine(0.0f, em() * 2.0f);
        const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w;
        if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
        ui::hint("%s", hint);
    }
}

} // namespace imageui
