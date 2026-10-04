#include "ImageEditPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>

#include <glad/gl.h>
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
#include "UiStyle.hpp"

namespace fs = std::filesystem;

namespace imageui {

float nextZoom(float z, int dir);   // ImageCanvas.cpp

namespace {

struct ToolDef { const char* name; const char* key; ImGuiKey imkey; const char* tip; const char* label; };
const ToolDef kTools[] = {
    {"Move",         "V", ImGuiKey_V, "Move what is selected (the whole layer with nothing selected). Drag, or the arrow buttons.", "Move"},
    {"Select",       "M", ImGuiKey_M, "Rectangle or ellipse. Click one corner, click the other -- or drag.", "Select"},
    {"Magic wand",   "W", ImGuiKey_W, "Select by colour: one click takes everything near the colour under it.", "Wand"},
    {"Brush",        "B", ImGuiKey_B, "Paint. Going over a spot again does not darken it.\nRight click: pick the colour under the pointer.", "Brush"},
    {"Eraser",       "E", ImGuiKey_E, "Paint transparency.", "Eraser"},
    {"Clone stamp",  "S", ImGuiKey_S, "Copy one part of the picture over another: pick the source, then paint.", "Clone"},
    {"Paint bucket", "K", ImGuiKey_K, "Fill the area of similar colour that was clicked.", "Bucket"},
    {"Gradient",     "G", ImGuiKey_G, "Foreground to background colour, from the first click to the second.", "Gradient"},
    {"Shapes",       "U", ImGuiKey_U, "Lines, rectangles and ellipses, outlined or filled. Click -- click, or drag.", "Shapes"},
    {"Text",         "T", ImGuiKey_T, "Click where the text goes, type it in the panel on the right.", "Text"},
    {"Eyedropper",   "I", ImGuiKey_I, "Click: foreground colour. Right click: background colour.", "Picker"},
    {"Hand",         "H", ImGuiKey_H, "Move the view. (The middle mouse button and Space do it with any tool.)", "Hand"},
};
static_assert(sizeof(kTools) / sizeof(kTools[0]) == std::size_t(Tool::Count));

const img::Color kPalette[] = {
    {0, 0, 0, 1}, {0.25f, 0.25f, 0.25f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.75f, 0.75f, 0.75f, 1}, {1, 1, 1, 1},
    {0.85f, 0.12f, 0.1f, 1}, {0.95f, 0.55f, 0.1f, 1}, {0.98f, 0.85f, 0.15f, 1}, {0.2f, 0.7f, 0.25f, 1}, {0.15f, 0.75f, 0.8f, 1},
    {0.15f, 0.35f, 0.85f, 1}, {0.5f, 0.25f, 0.75f, 1}, {0.85f, 0.25f, 0.6f, 1}, {0.9f, 0.72f, 0.6f, 1}, {0.6f, 0.25f, 0.2f, 1},
    {0.36f, 0.25f, 0.15f, 1}, {0.76f, 0.6f, 0.42f, 1}, {0.86f, 0.78f, 0.6f, 1}, {0.55f, 0.53f, 0.5f, 1}, {0.35f, 0.33f, 0.3f, 1},
    {0.42f, 0.45f, 0.2f, 1}, {0.3f, 0.4f, 0.2f, 1}, {0.18f, 0.28f, 0.14f, 1}, {0.53f, 0.73f, 0.9f, 1}, {0.15f, 0.35f, 0.45f, 1},
};

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
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
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

} // namespace

// --- Life -----------------------------------------------------------------------------

ImageEditor::ImageEditor(Deps d) : m_d(d) {}

ImageEditor::~ImageEditor() {
    for (auto& v : m_views) if (v->tex) m_retired.push_back(v->tex);
    if (m_checker) m_retired.push_back(m_checker);
    if (!m_retired.empty()) glDeleteTextures(GLsizei(m_retired.size()), m_retired.data());
}

bool ImageEditor::hasKeyboard() const { return m_focusFrame >= ImGui::GetFrameCount() - 1; }

img::Document* ImageEditor::document() { View* v = cur(); return v ? &v->doc : nullptr; }

void ImageEditor::setTool(Tool t) {
    if (t == m_tool) return;
    textEnd(true);
    if (View* v = cur()) cancelGesture(*v);
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
    m_d.status = "Opened " + d.title() + " (" + std::to_string(d.width()) + " x " + std::to_string(d.height()) + ")";
    return true;
}

void ImageEditor::openDialog() {
    std::string out;
    if (ed::pickFile(out, startDir(), "Images", "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif")) open(out);
}

bool ImageEditor::save(View& v, bool as) {
    std::string path = v.doc.path();
    if (as || path.empty() || !saveable(path)) {
        const std::string dir = path.empty() ? startDir() : fs::path(path).parent_path().generic_string();
        const std::string name = path.empty() ? std::string("untitled.png") : fs::path(path).stem().string() + ".png";
        std::string out;
        if (!ed::saveFile(out, dir, name, "Images (png, jpg, tga, bmp)", "*.png;*.jpg;*.jpeg;*.tga;*.bmp", "png"))
            return false;
        path = out;
    }
    std::string err;
    if (!v.doc.save(path, err)) { m_d.status = "Not saved: " + err; return false; }
    m_lastDir = fs::path(path).parent_path().generic_string();
    v.closeArmed = false;
    std::string msg = "Saved " + v.doc.title();
    if (v.doc.layerCount() > 1) msg += " (" + std::to_string(v.doc.layerCount()) + " layers flattened into the file; they stay here)";
    const std::string e = lower(fs::path(path).extension().string());
    if (e == ".jpg" || e == ".jpeg") msg += " -- JPEG keeps no transparency";
    m_d.status = msg;
    if (!projectDir().empty() && lower(path).rfind(lower(projectDir()), 0) == 0 &&
        std::find(m_images.begin(), m_images.end(), path) == m_images.end())
        m_scannedFor = "\x01";   // a new picture in the project: list it
    return true;
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
        if (m_pane == Pane::Filter) closePane(v, false);
        m_pane = Pane::None;
    }
    if (m_text.on && m_text.viewId == v.id) m_text.on = false;
    if (v.tex) m_retired.push_back(v.tex);
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
    if (m_recent.size() > 10) m_recent.resize(10);
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
    m_d.status = "Pasted onto a new layer -- the Move tool (V) puts it in place.";
}

void ImageEditor::openFilter(View& v, img::Filter f) {
    cancelGesture(v);
    if (m_pane == Pane::Filter) closePane(v, false);
    const img::FilterInfo& fi = img::filterInfo(f);
    if (fi.params == 0) {
        v.doc.filter(f, nullptr);
        m_d.status = std::string(fi.name) + (v.doc.hasSelection() ? " (inside the selection)." : ".");
        return;
    }
    m_pane = Pane::Filter;
    m_filter = f;
    for (int k = 0; k < 3; ++k) m_fp[k] = fi.def[k];
    v.doc.beginEdit(fi.name);
    v.doc.filter(f, m_fp, m_seed);
}

void ImageEditor::closePane(View& v, bool apply) {
    if (m_pane == Pane::Filter) {
        if (apply) v.doc.endEdit(); else v.doc.cancelEdit();
    }
    m_pane = Pane::None;
}

// --- The window -----------------------------------------------------------------------

void ImageEditor::panel(bool& show) {
    // Textures retired last frame were drawn by then; now they can go.
    if (!m_retired.empty()) {
        glDeleteTextures(GLsizei(m_retired.size()), m_retired.data());
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
        glGenTextures(1, &m_checker);
        glBindTexture(GL_TEXTURE_2D, m_checker);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
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
                const std::string label = w.doc.title() + "###pic" + std::to_string(w.id);
                bool open = true;
                ImGuiTabItemFlags fl = w.doc.modified() ? ImGuiTabItemFlags_UnsavedDocument : 0;
                if (m_selectTab == i) fl |= ImGuiTabItemFlags_SetSelected;
                if (ImGui::BeginTabItem(label.c_str(), &open, fl)) {
                    if (m_cur != i && m_selectTab < 0) {
                        if (View* o = cur()) {
                            cancelGesture(*o);
                            if (m_pane != Pane::None) closePane(*o, false);
                        }
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
            topBar(*v);
            const float statusH = ImGui::GetFrameHeightWithSpacing() + 4.0f;
            const float availW = ImGui::GetContentRegionAvail().x;
            const float toolW = std::clamp(availW * 0.13f, em() * 8.0f, em() * 11.0f);
            const float sideW = std::clamp(availW * 0.25f, em() * 15.0f, em() * 21.0f);
            ImGui::BeginChild("##tools", ImVec2(toolW, -statusH), ImGuiChildFlags_None);
            toolColumn();
            colorSwatches();
            ImGui::EndChild();
            ImGui::SameLine();
            const float canvasW = ImGui::GetContentRegionAvail().x - sideW - ImGui::GetStyle().ItemSpacing.x;
            ImGui::BeginChild("##canvas", ImVec2(std::max(100.0f, canvasW), -statusH), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            canvas(*v);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##side", ImVec2(0.0f, -statusH), ImGuiChildFlags_None);
            sidePanel(*v);
            ImGui::EndChild();
            statusLine(*v);
        }
    }
    ImGui::End();
    // While the window has the keyboard, claim it the way a text field does:
    // every editor shortcut (scene undo, Q/W/E, V, G, camera keys ...) is read
    // before the next UI frame and stands aside for io.WantTextInput.
    if (focused) ImGui::GetIO().WantTextInput = true;
}

void ImageEditor::shortcuts(View* v) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;   // a field of this window is being typed into
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    if (ctrl) {
        if (key(ImGuiKey_O)) openDialog();
        if (key(ImGuiKey_N)) m_pane = Pane::New;
        if (key(ImGuiKey_V) && shift) paste(v, true);
        if (!v) return;
        img::Document& d = v->doc;
        if (m_pane == Pane::Filter) return;   // apply or cancel the filter first
        if (key(ImGuiKey_Z)) { cancelGesture(*v); if (shift) d.redo(); else d.undo(); }
        if (key(ImGuiKey_Y)) { cancelGesture(*v); d.redo(); }
        if (key(ImGuiKey_S)) save(*v, shift);
        if (key(ImGuiKey_C)) copy(*v, shift, false);
        if (key(ImGuiKey_X)) copy(*v, false, true);
        if (key(ImGuiKey_V) && !shift) paste(v, false);
        if (key(ImGuiKey_A)) d.selectAll();
        if (key(ImGuiKey_D)) d.deselect();
        if (key(ImGuiKey_I) && shift) d.invertSelection();
        if (key(ImGuiKey_J)) d.duplicateLayer();
        if (key(ImGuiKey_E)) d.mergeDown();
        return;
    }
    if (key(ImGuiKey_Escape)) {
        if (v && m_pane != Pane::None) closePane(*v, false);
        else if (m_text.on) textEnd(false);
        else if (v) cancelGesture(*v);
    }
    if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) {
        if (v && m_pane == Pane::Filter) closePane(*v, true);
    }
    if (!v) return;
    for (int t = 0; t < int(Tool::Count); ++t)
        if (key(kTools[t].imkey)) setTool(Tool(t));
    if (key(ImGuiKey_X)) std::swap(m_fg, m_bg);
    if (key(ImGuiKey_D)) { m_fg = {0, 0, 0, 1}; m_bg = {1, 1, 1, 1}; }
    if (key(ImGuiKey_LeftBracket)) m_brush.radius = std::max(0.5f, m_brush.radius / 1.25f);
    if (key(ImGuiKey_RightBracket)) m_brush.radius = std::min(500.0f, m_brush.radius * 1.25f);
    if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) zoomAt(*v, nextZoom(v->zoom, +1), m_center);
    if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) zoomAt(*v, nextZoom(v->zoom, -1), m_center);
    if (key(ImGuiKey_0)) v->fitPending = true;
    if (key(ImGuiKey_1)) zoomAt(*v, 1.0f, m_center);
    if (m_pane == Pane::Filter) return;
    if (key(ImGuiKey_Delete) || key(ImGuiKey_Backspace)) v->doc.clearSelection();
    if (m_tool == Tool::Move) {
        const int s = shift ? 10 * std::max(1, m_nudge) : std::max(1, m_nudge);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  v->doc.moveSelected(-s, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) v->doc.moveSelected(s, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    v->doc.moveSelected(0, -s);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  v->doc.moveSelected(0, s);
    }
}

void ImageEditor::menuBar() {
    if (!ImGui::BeginMenuBar()) return;
    View* v = cur();
    const bool has = v != nullptr;
    const bool busy = m_pane == Pane::Filter;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New picture...", "Ctrl+N")) m_pane = Pane::New;
        if (ImGui::MenuItem("Open...", "Ctrl+O")) openDialog();
        if (ImGui::BeginMenu("Project pictures", !m_images.empty())) {
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
        if (ImGui::MenuItem("Paste as new picture", "Ctrl+Shift+V")) paste(v, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, has && !busy)) save(*v, false);
        if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S", false, has && !busy)) save(*v, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Close", nullptr, false, has)) requestClose(m_cur);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit", has && !busy)) {
        img::Document& d = v->doc;
        const std::string ul = d.canUndo() ? "Undo " + d.undoLabel() : std::string("Undo");
        const std::string rl = d.canRedo() ? "Redo " + d.redoLabel() : std::string("Redo");
        if (ImGui::MenuItem(ul.c_str(), "Ctrl+Z", false, d.canUndo())) { cancelGesture(*v); d.undo(); }
        if (ImGui::MenuItem(rl.c_str(), "Ctrl+Y", false, d.canRedo())) { cancelGesture(*v); d.redo(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X")) copy(*v, false, true);
        if (ImGui::MenuItem("Copy", "Ctrl+C")) copy(*v, false, false);
        if (ImGui::MenuItem("Copy merged", "Ctrl+Shift+C")) copy(*v, true, false);
        if (ImGui::MenuItem("Paste", "Ctrl+V")) paste(v, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Fill with foreground colour")) { d.fillSelection(m_fg); rememberColor(m_fg); }
        if (ImGui::MenuItem("Clear", "Delete")) d.clearSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Select all", "Ctrl+A")) d.selectAll();
        if (ImGui::MenuItem("Deselect", "Ctrl+D", false, d.hasSelection())) d.deselect();
        if (ImGui::MenuItem("Invert selection", "Ctrl+Shift+I")) d.invertSelection();
        if (ImGui::MenuItem("Select the layer's pixels")) d.selectLayerAlpha(img::SelOp::Replace);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Image", has && !busy)) {
        img::Document& d = v->doc;
        if (ImGui::MenuItem("Image size...")) { m_pane = Pane::Resize; m_resW = d.width(); m_resH = d.height(); }
        if (ImGui::MenuItem("Canvas size...")) { m_pane = Pane::Canvas; m_resW = d.width(); m_resH = d.height(); }
        if (ImGui::MenuItem("Crop to selection", nullptr, false, d.hasSelection())) d.crop(d.selectionBounds());
        ImGui::Separator();
        if (ImGui::MenuItem("Rotate 90 right")) d.rotate90(true);
        if (ImGui::MenuItem("Rotate 90 left")) d.rotate90(false);
        if (ImGui::MenuItem("Rotate 180")) d.rotate180();
        if (ImGui::MenuItem("Flip horizontal")) d.flipImage(true);
        if (ImGui::MenuItem("Flip vertical")) d.flipImage(false);
        ImGui::Separator();
        if (ImGui::MenuItem("Flatten", nullptr, false, d.layerCount() > 1)) d.flatten();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Layer", has && !busy)) {
        img::Document& d = v->doc;
        if (ImGui::MenuItem("New layer")) d.addLayer();
        if (ImGui::MenuItem("Duplicate", "Ctrl+J")) d.duplicateLayer();
        if (ImGui::MenuItem("Delete", nullptr, false, d.layerCount() > 1)) d.deleteLayer();
        if (ImGui::MenuItem("Merge down", "Ctrl+E", false, d.active() > 0)) d.mergeDown();
        ImGui::Separator();
        if (ImGui::MenuItem("Flip layer horizontal")) d.flipLayer(true);
        if (ImGui::MenuItem("Flip layer vertical")) d.flipLayer(false);
        if (ImGui::MenuItem("Offset by half (check tile seams)")) d.offsetLayer(d.width() / 2, d.height() / 2);
        ImGui::EndMenu();
    }
    for (const char* group : {"Adjust", "Filter"}) {
        if (ImGui::BeginMenu(group, has && !busy)) {
            for (int f = 0; f < int(img::Filter::Count); ++f) {
                const img::FilterInfo& fi = img::filterInfo(img::Filter(f));
                if (std::strcmp(fi.group, group) != 0) continue;
                const std::string label = std::string(fi.name) + (fi.params ? "..." : "");
                if (ImGui::MenuItem(label.c_str())) openFilter(*v, img::Filter(f));
                ImGui::SetItemTooltip("%s", fi.tip);
            }
            ImGui::EndMenu();
        }
    }
    ImGui::EndMenuBar();
}

// The New picture controls -- in the welcome page and the New pane. True when made.
static bool newControls(int& w, int& h, int& fill, const img::Color& fg, std::function<void(int, int, img::Color)> make);

void ImageEditor::welcome() {
    ImGui::Spacing();
    ui::title("Image editor");
    ui::hint("Paint, retouch and resize the pictures a project is made of. Open one below, drop a file\n"
             "on this window, or start a new one. Saving writes the file; materials using it update.");
    ImGui::Spacing();
    const float colW = std::max(360.0f, em() * 22.0f);
    ImGui::BeginChild("##new", ImVec2(colW, 0.0f), ImGuiChildFlags_Borders);
    ui::sectionText("New picture");
    newControls(m_newW, m_newH, m_newFill, m_fg, [&](int w, int h, img::Color c) { newImage(w, h, c); });
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##open", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    ui::sectionText("Open");
    if (bigButton("Open file...", "PNG, JPG, TGA, BMP (PSD and GIF: the first picture)", across(2))) openDialog();
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

void ImageEditor::topBar(View& v) {
    img::Document& d = v.doc;
    const bool busy = m_pane == Pane::Filter;
    ImGui::BeginDisabled(busy);
    if (bigButton("New", "New picture (Ctrl+N)")) m_pane = Pane::New;
    ImGui::SameLine();
    if (bigButton("Open", "Open a picture (Ctrl+O)")) openDialog();
    ImGui::SameLine();
    if (bigButton("Save", "Save (Ctrl+S). Layers are flattened into the file and stay here.")) save(v, false);
    ImGui::SameLine(0.0f, em() * 1.2f);
    ImGui::BeginDisabled(!d.canUndo());
    if (bigButton("Undo", d.canUndo() ? ("Undo " + d.undoLabel() + " (Ctrl+Z)").c_str() : nullptr)) { cancelGesture(v); d.undo(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!d.canRedo());
    if (bigButton("Redo", d.canRedo() ? ("Redo " + d.redoLabel() + " (Ctrl+Y)").c_str() : nullptr)) { cancelGesture(v); d.redo(); }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, em() * 1.2f);
    if (bigButton(" - ", "Zoom out (-)")) zoomAt(v, nextZoom(v.zoom, -1), m_center);
    ImGui::SameLine();
    char z[32];
    std::snprintf(z, sizeof z, v.zoom >= 1.0f ? "%.0f %%" : "%.1f %%", v.zoom * 100.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::SetNextItemWidth(em() * 4.0f);
    ImGui::TextUnformatted(z);
    ImGui::SameLine();
    if (bigButton(" + ", "Zoom in (+). The mouse wheel zooms where the pointer is.")) zoomAt(v, nextZoom(v.zoom, +1), m_center);
    ImGui::SameLine();
    if (bigButton("Fit", "The whole picture (0)")) v.fitPending = true;
    ImGui::SameLine();
    if (bigButton("1:1", "One picture pixel per screen pixel (1)")) zoomAt(v, 1.0f, m_center);
    if (v.closeArmed) {
        ImGui::SameLine(0.0f, em() * 1.2f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.2f, 0.15f, 1.0f));
        if (bigButton("Close without saving")) { closeView(m_cur); ImGui::PopStyleColor(); return; }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (bigButton("Keep it open")) v.closeArmed = false;
    }
}

void ImageEditor::toolColumn() {
    // Two to a row, each a big target; the key is in the tooltip.
    const float w = across(2);
    for (int t = 0; t < int(Tool::Count); ++t) {
        const ToolDef& td = kTools[t];
        char label[64];
        std::snprintf(label, sizeof label, "%s##tool%d", td.label, t);
        if (t % 2) ImGui::SameLine();
        if (toggle(label, m_tool == Tool(t), w)) setTool(Tool(t));
        ImGui::SetItemTooltip("%s (%s)\n%s", td.name, td.key, td.tip);
    }
}

void ImageEditor::colorPopup(const char* id, img::Color& c) {
    if (!ImGui::BeginPopup(id)) return;
    ImGui::SetNextItemWidth(em() * 15.0f);
    ImGui::ColorPicker4("##picker", &c.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs |
                                              ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview);
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
    ImGui::EndPopup();
}

void ImageEditor::colorSwatches() {
    ImGui::Spacing();
    ui::sectionText("Colours");
    const float sw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::ColorButton("Foreground##fg", toIm(m_fg), ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(sw, em() * 2.6f)))
        ImGui::OpenPopup("##fgpop");
    ImGui::SetItemTooltip("Foreground %s -- click to change", img::hexOf(m_fg).c_str());
    ImGui::SameLine();
    if (ImGui::ColorButton("Background##bg", toIm(m_bg), ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(sw, em() * 2.6f)))
        ImGui::OpenPopup("##bgpop");
    ImGui::SetItemTooltip("Background %s -- click to change", img::hexOf(m_bg).c_str());
    colorPopup("##fgpop", m_fg);
    colorPopup("##bgpop", m_bg);
    if (bigButton("Swap", "Swap foreground and background (X)", sw)) std::swap(m_fg, m_bg);
    ImGui::SameLine();
    if (bigButton("B / W", "Black and white (D)", sw)) { m_fg = {0, 0, 0, 1}; m_bg = {1, 1, 1, 1}; }
    ImGui::Spacing();
    // The palette: a click takes the colour as foreground, a right click as background.
    const int perRow = 5;
    const float cs = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (perRow - 1)) / perRow);
    auto swatch = [&](const img::Color& c, int i) {
        if (i % perRow) ImGui::SameLine();
        ImGui::PushID(i);
        if (ImGui::ColorButton("##sw", toIm(c), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(cs, cs)))
            m_fg = c;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m_bg = c;
        ImGui::PopID();
    };
    for (int i = 0; i < int(sizeof kPalette / sizeof kPalette[0]); ++i) swatch(kPalette[i], i);
    if (!m_recent.empty()) {
        ImGui::Spacing();
        ui::hint("Used lately");
        for (int i = 0; i < int(m_recent.size()); ++i) swatch(m_recent[std::size_t(i)], 100 + i);
    }
}

void ImageEditor::sidePanel(View& v) {
    if (m_pane != Pane::None) {
        panePanel(v);
        ImGui::Separator();
        ImGui::Spacing();
    }
    ui::sectionText(kTools[int(m_tool)].name);
    toolOptions(v);
    ImGui::Spacing();
    if (ui::header("Layers", ImGuiTreeNodeFlags_DefaultOpen)) layersPanel(v);
    if (ui::header("History")) historyPanel(v);
}

void ImageEditor::toolOptions(View& v) {
    img::Document& d = v.doc;
    const float full = ImGui::GetContentRegionAvail().x;
    auto opButtons = [&] {
        static const char* names[] = {"Replace", "Add", "Subtract", "Intersect"};
        static const char* tips[] = {"Replace the selection", "Add to the selection", "Take away from the selection",
                                     "Keep only where both are"};
        const float w = across(4);
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            if (toggle(names[i], int(m_selOp) == i, w, tips[i])) m_selOp = img::SelOp(i);
        }
    };
    auto selectButtons = [&] {
        const float w = across(2);
        if (bigButton("Select all", "Ctrl+A", w)) d.selectAll();
        ImGui::SameLine();
        ImGui::BeginDisabled(!d.hasSelection());
        if (bigButton("Deselect", "Ctrl+D", w)) d.deselect();
        ImGui::EndDisabled();
        if (bigButton("Invert", "Ctrl+Shift+I", w)) d.invertSelection();
        ImGui::SameLine();
        ImGui::BeginDisabled(!d.hasSelection());
        if (bigButton("Crop to it", "Cut the picture down to the selection's box", w)) d.crop(d.selectionBounds());
        ImGui::EndDisabled();
    };
    auto snapRow = [&] {
        static const int steps[] = {1, 2, 4, 8, 16, 32, 64, 128};
        int idx = 0;
        while (idx < 7 && steps[idx] < m_snap) ++idx;
        float fi = float(idx);
        char label[32];
        if (m_snap <= 1) std::snprintf(label, sizeof label, "whole pixels");
        else std::snprintf(label, sizeof label, "%d px grid", m_snap);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Snap");
        ImGui::SameLine(em() * 6.0f);
        if (ui::stepper("snap", fi, 1.0f, 0.0f, 7.0f, label, ImGui::GetContentRegionAvail().x))
            m_snap = steps[std::clamp(int(fi), 0, 7)];
        ImGui::SetItemTooltip("Corners, shapes, text and moves land on this grid.");
    };
    auto constrainRow = [&](const char* what) {
        ImGui::Checkbox(what, &m_constrain);
        ImGui::SetItemTooltip("The same as holding Shift.");
    };
    auto amount = [&](const char* label, const char* id, float& v01, float step, const char* fmt) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(em() * 6.0f);
        float p = v01 * 100.0f;
        if (ui::stepper(id, p, step, 0.0f, 100.0f, fmt, ImGui::GetContentRegionAvail().x)) v01 = p / 100.0f;
    };
    auto value = [&](const char* label, const char* id, float& val, float step, float lo, float hi, const char* fmt) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(em() * 6.0f);
        return ui::stepper(id, val, step, lo, hi, fmt, ImGui::GetContentRegionAvail().x);
    };
    auto brushSize = [&] {
        const float r = m_brush.radius;
        const float step = r < 5 ? 0.5f : r < 20 ? 1.0f : r < 50 ? 5.0f : r < 200 ? 10.0f : 25.0f;
        float diam = r * 2.0f;
        if (value("Size", "size", diam, step * 2.0f, 1.0f, 1000.0f, "%.0f px")) m_brush.radius = diam * 0.5f;
        ImGui::SetItemTooltip("[ and ] change it too.");
    };

    switch (m_tool) {
    case Tool::Brush:
    case Tool::Eraser:
    case Tool::Clone:
        if (m_tool == Tool::Clone) {
            if (bigButton(m_clonePick ? "Click the source in the picture..." : "Pick source", nullptr, -1.0f)) {
                m_clonePick = true;
            }
            ui::hint(m_cloneHasSrc ? "The source moves along with the brush." : "First click the picture where to copy from.");
        }
        brushSize();
        amount("Hardness", "hard", m_brush.hardness, 10.0f, "%.0f %%");
        amount("Opacity", "opa", m_brush.opacity, 5.0f, "%.0f %%");
        value("Steady", "stab", m_stabilize, 2.0f, 0.0f, 120.0f, m_stabilize <= 0 ? "off" : "%.0f px");
        ImGui::SetItemTooltip("Stabiliser: the brush follows the pointer on a string this long,\n"
                              "so a shaking hand draws a calm line. 0 = off.");
        ImGui::Checkbox("Lines from click to click", &m_lineMode);
        ImGui::SetItemTooltip("Each click paints a straight line from the last one -- no dragging at all.\n"
                              "Esc starts afresh.");
        if (m_lineMode) constrainRow("Only 45 degree steps");
        break;
    case Tool::Select:
        {
            const float w = across(2);
            if (toggle("Rectangle", !m_ellipse, w)) m_ellipse = false;
            ImGui::SameLine();
            if (toggle("Ellipse", m_ellipse, w)) m_ellipse = true;
        }
        opButtons();
        snapRow();
        constrainRow("Square / circle");
        ImGui::Spacing();
        selectButtons();
        break;
    case Tool::Wand:
        amount("Tolerance", "tol", m_tolerance, 2.0f, "%.0f %%");
        ImGui::Checkbox("Only joined pixels", &m_contiguous);
        ImGui::Checkbox("Look at all layers", &m_sampleMerged);
        opButtons();
        ImGui::Spacing();
        selectButtons();
        break;
    case Tool::Fill:
        amount("Tolerance", "tol", m_tolerance, 2.0f, "%.0f %%");
        amount("Opacity", "fop", m_fillOpacity, 5.0f, "%.0f %%");
        ImGui::Checkbox("Only joined pixels", &m_contiguous);
        ImGui::Checkbox("Look at all layers", &m_sampleMerged);
        if (bigButton(d.hasSelection() ? "Fill the selection" : "Fill the whole layer", nullptr, -1.0f)) {
            d.fillSelection(m_fg);
            rememberColor(m_fg);
        }
        break;
    case Tool::Gradient: {
        const float w = across(2);
        if (toggle("Linear", !m_radial, w)) m_radial = false;
        ImGui::SameLine();
        if (toggle("Radial", m_radial, w)) m_radial = true;
        amount("Opacity", "gop", m_fillOpacity, 5.0f, "%.0f %%");
        snapRow();
        constrainRow("Only 45 degree steps");
        ui::hint("From the foreground colour at the first click\nto the background colour at the second.");
        break;
    }
    case Tool::Shape: {
        static const char* names[] = {"Line", "Rectangle", "Filled rect.", "Ellipse", "Filled ellipse"};
        for (int i = 0; i < 5; ++i) {
            if (i != 0 && i != 3) ImGui::SameLine();
            const float w = i == 0 ? -1.0f : across(i == 1 || i == 3 ? 2 : 1);
            if (toggle(names[i], m_shape == i, w)) m_shape = i;
        }
        if (m_shape != 2 && m_shape != 4) value("Width", "lw", m_lineWidth, m_lineWidth < 10 ? 1.0f : 5.0f, 1.0f, 400.0f, "%.0f px");
        amount("Opacity", "sop", m_brush.opacity, 5.0f, "%.0f %%");
        snapRow();
        constrainRow(m_shape == 0 ? "Only 45 degree steps" : "Square / circle");
        break;
    }
    case Tool::Text: {
        ui::hint(m_text.on ? "Type below. Click the picture again to move it." : "Click the picture where the text goes.");
        if (m_focusText) { ImGui::SetKeyboardFocusHere(); m_focusText = false; }
        if (ImGui::InputTextMultiline("##text", m_textBuf, sizeof m_textBuf, ImVec2(-1.0f, em() * 5.0f)))
            m_text.dirty = true;
        if (value("Size", "tsize", m_textSize, m_textSize < 24 ? 2.0f : m_textSize < 100 ? 4.0f : 16.0f, 6.0f, 1000.0f, "%.0f px"))
            m_text.dirty = true;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Font");
        ImGui::SameLine(em() * 6.0f);
        if (bigButton((m_fontName + "##font").c_str(), "Pick a font", -1.0f)) {
            m_fontSearch[0] = '\0';
            ImGui::OpenPopup("##fonts");
        }
        ImGui::SetNextWindowSize(ImVec2(em() * 20.0f, em() * 26.0f));
        if (ImGui::BeginPopup("##fonts")) {
            ui::searchBox("##fs", m_fontSearch, sizeof m_fontSearch, "Search fonts...");
            ImGui::BeginChild("##fl");
            for (const auto& f : img::systemFonts()) {
                if (!ui::icontains(f.name.c_str(), m_fontSearch)) continue;
                if (ImGui::Selectable(f.name.c_str(), f.name == m_fontName, 0, ImVec2(0.0f, em() * 1.5f))) {
                    m_fontName = f.name;
                    m_font = f.path;
                    m_text.dirty = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        snapRow();
        if (m_text.on) {
            const float w = across(2);
            if (bigButton("Done", "Keep the text as a layer of its own", w)) textEnd(true);
            ImGui::SameLine();
            if (bigButton("Cancel", "Throw the text away", w)) textEnd(false);
        }
        break;
    }
    case Tool::Eyedropper:
        ImGui::Checkbox("Look at all layers", &m_sampleMerged);
        ui::hint("Click: foreground colour.\nRight click: background colour.");
        break;
    case Tool::Move: {
        ui::hint(d.hasSelection() ? "Moves what is selected on this layer." : "Moves the whole layer.");
        snapRow();
        constrainRow("Only straight across or down");
        float n = float(m_nudge);
        if (value("Step", "nudge", n, 1.0f, 1.0f, 256.0f, "%.0f px")) m_nudge = int(n);
        const float bw = std::min(em() * 4.0f, across(3));
        const float pad = (ImGui::GetContentRegionAvail().x - bw * 3.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f) * 0.5f;
        auto at = [&] { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad); };
        at(); ImGui::Dummy(ImVec2(bw, 1)); ImGui::SameLine();
        if (bigButton("Up", "Arrow keys too (Shift: ten steps)", bw)) d.moveSelected(0, -m_nudge);
        at();
        if (bigButton("Left", nullptr, bw)) d.moveSelected(-m_nudge, 0);
        ImGui::SameLine();
        if (bigButton("Down", nullptr, bw)) d.moveSelected(0, m_nudge);
        ImGui::SameLine();
        if (bigButton("Right", nullptr, bw)) d.moveSelected(m_nudge, 0);
        ImGui::Spacing();
        if (bigButton("Offset layer by half", "Wraps the layer round by half its size -- the seams of a\n"
                                              "texture tile come to the middle, where you can see them.", -1.0f))
            d.offsetLayer(d.width() / 2, d.height() / 2);
        break;
    }
    case Tool::Hand:
        ui::hint("Drag to move the view. The wheel zooms.");
        break;
    default:
        break;
    }
    (void)full;
}

void ImageEditor::layersPanel(View& v) {
    img::Document& d = v.doc;
    const float rowH = em() * 1.9f;
    ImGui::BeginChild("##layers", ImVec2(0.0f, std::min(rowH * float(d.layerCount()) + em(), em() * 14.0f)),
                      ImGuiChildFlags_Borders);
    for (int i = d.layerCount() - 1; i >= 0; --i) {
        const img::Layer& l = d.layer(i);
        ImGui::PushID(i);
        if (bigButton(l.visible ? "Shown" : "Hidden", "Show or hide the layer", em() * 4.2f)) d.setVisible(i, !l.visible);
        ImGui::SameLine();
        if (!l.visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        char label[160];
        if (l.opacity < 1.0f || l.blend != img::Blend::Normal)
            std::snprintf(label, sizeof label, "%s   %.0f%% %s", l.name.c_str(), l.opacity * 100.0f,
                          l.blend != img::Blend::Normal ? img::blendName(l.blend) : "");
        else
            std::snprintf(label, sizeof label, "%s", l.name.c_str());
        if (ImGui::Selectable(label, i == d.active(), 0, ImVec2(0.0f, rowH - 4.0f))) {
            cancelGesture(v);
            d.setActive(i);
        }
        if (!l.visible) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
    const float w = across(3);
    if (bigButton("New##layer", "A new empty layer above this one", w)) d.addLayer();
    ImGui::SameLine();
    if (bigButton("Copy", "Duplicate the layer (Ctrl+J)", w)) d.duplicateLayer();
    ImGui::SameLine();
    ImGui::BeginDisabled(d.layerCount() < 2);
    if (bigButton("Delete", nullptr, w)) d.deleteLayer();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(d.active() >= d.layerCount() - 1);
    if (bigButton("Up", "Move the layer up", w)) d.moveLayer(+1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(d.active() <= 0);
    if (bigButton("Down", "Move the layer down", w)) d.moveLayer(-1);
    ImGui::SameLine();
    if (bigButton("Merge", "Merge into the layer below (Ctrl+E)", w)) d.mergeDown();
    ImGui::EndDisabled();

    // The active layer's own settings.
    const int a = d.active();
    if (m_nameFor != a || !ImGui::IsAnyItemActive()) {
        if (m_nameFor != a || std::strcmp(m_layerName, d.layer(a).name.c_str()) != 0) {
            std::snprintf(m_layerName, sizeof m_layerName, "%s", d.layer(a).name.c_str());
            m_nameFor = a;
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(em() * 6.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##lname", m_layerName, sizeof m_layerName);
    if (ImGui::IsItemDeactivatedAfterEdit() && m_layerName[0]) d.rename(a, m_layerName);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Blend");
    ImGui::SameLine(em() * 6.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##blend", img::blendName(d.layer(a).blend), ImGuiComboFlags_HeightLargest)) {
        for (int b = 0; b < int(img::Blend::Count); ++b)
            if (ImGui::Selectable(img::blendName(img::Blend(b)), int(d.layer(a).blend) == b, 0, ImVec2(0.0f, em() * 1.5f)))
                d.setBlend(a, img::Blend(b));
        ImGui::EndCombo();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Opacity");
    ImGui::SameLine(em() * 6.0f);
    float op = d.layer(a).opacity * 100.0f;
    if (ui::stepper("lop", op, 5.0f, 0.0f, 100.0f, "%.0f %%", ImGui::GetContentRegionAvail().x)) d.setOpacity(a, op / 100.0f);
}

void ImageEditor::historyPanel(View& v) {
    img::Document& d = v.doc;
    int applied = 0;
    const std::vector<std::string> h = d.history(applied);
    ImGui::BeginChild("##hist", ImVec2(0.0f, em() * 12.0f), ImGuiChildFlags_Borders);
    const float rowH = em() * 1.5f;
    if (ImGui::Selectable("Opened", applied == 0, 0, ImVec2(0.0f, rowH))) { cancelGesture(v); d.jumpTo(0); }
    for (int i = 0; i < int(h.size()); ++i) {
        ImGui::PushID(i);
        const bool undone = i >= applied;
        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(h[std::size_t(i)].c_str(), applied == i + 1, 0, ImVec2(0.0f, rowH))) {
            cancelGesture(v);
            d.jumpTo(i + 1);
        }
        if (undone) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (ImGui::GetScrollMaxY() > 0.0f && applied == int(h.size())) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ui::hint("Click a step to go back to it; the later ones stay until you do something new.");
}

void ImageEditor::panePanel(View& v) {
    img::Document& d = v.doc;
    const float w2 = across(2);
    switch (m_pane) {
    case Pane::Filter: {
        if (!d.editing()) { m_pane = Pane::None; return; }   // something else took the edit
        const img::FilterInfo& fi = img::filterInfo(m_filter);
        ui::sectionText(fi.name);
        ui::hint("%s", fi.tip);
        bool changed = false;
        for (int k = 0; k < fi.params; ++k) {
            ImGui::PushID(k);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(fi.label[k]);
            ImGui::SameLine(em() * 7.5f);
            changed |= ui::stepper("p", m_fp[k], fi.step[k], fi.lo[k], fi.hi[k], fi.fmt[k], ImGui::GetContentRegionAvail().x);
            ImGui::PopID();
        }
        if (m_filter == img::Filter::Noise && bigButton("New grain", nullptr, -1.0f)) { ++m_seed; changed = true; }
        if (changed) d.filter(m_filter, m_fp, m_seed);
        if (d.hasSelection()) ui::hint("Only inside the selection.");
        if (bigButton("Apply", "Enter", w2)) closePane(v, true);
        ImGui::SameLine();
        if (bigButton("Cancel", "Esc", w2)) closePane(v, false);
        break;
    }
    case Pane::Resize:
    case Pane::Canvas: {
        const bool resize = m_pane == Pane::Resize;
        ui::sectionText(resize ? "Image size" : "Canvas size");
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
            const float w4 = across(4);
            const struct { const char* l; double k; } ks[] = {{"25 %", 0.25}, {"50 %", 0.5}, {"200 %", 2.0}, {"400 %", 4.0}};
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine();
                if (bigButton(ks[i].l, nullptr, w4)) {
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
            ImGui::TextUnformatted("Keep the picture at");
            const float bw = std::min(em() * 3.2f, across(3));
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x) {
                    if (x) ImGui::SameLine();
                    ImGui::PushID(y * 3 + x);
                    if (toggle(m_ancX == x && m_ancY == y ? "o" : " ", m_ancX == x && m_ancY == y, bw)) { m_ancX = x; m_ancY = y; }
                    ImGui::PopID();
                }
        }
        ImGui::Text("%d x %d  ->  %d x %d", ow, oh, m_resW, m_resH);
        if (bigButton("Apply", nullptr, w2)) {
            if (resize) d.resize(m_resW, m_resH); else d.canvasSize(m_resW, m_resH, m_ancX, m_ancY);
            v.fitPending = true;
            m_pane = Pane::None;
        }
        ImGui::SameLine();
        if (bigButton("Cancel", "Esc", w2)) m_pane = Pane::None;
        break;
    }
    case Pane::New:
        ui::sectionText("New picture");
        if (newControls(m_newW, m_newH, m_newFill, m_fg, [&](int w, int h, img::Color c) { newImage(w, h, c); }))
            m_pane = Pane::None;
        if (bigButton("Cancel", nullptr, -1.0f)) m_pane = Pane::None;
        break;
    default:
        break;
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
    if (m_tool == Tool::Brush || m_tool == Tool::Fill || m_tool == Tool::Shape) {
        ImGui::SameLine(0.0f, em() * 2.0f);
        ui::hint("right click: pick colour");
    }
}

} // namespace imageui
