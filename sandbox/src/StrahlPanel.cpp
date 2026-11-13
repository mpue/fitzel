#include "StrahlPanel.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <imgui.h>
#include <imgui_internal.h>

#include <stb_image_write.h>
#include <tinyexr.h>

#include <fitzel/graphics/Texture.hpp>
#include <fitzel/ui/ImGuiTexture.hpp>

#include "CodeEditor.hpp"
#include "FolderDialog.hpp"
#include "PathTrace.hpp"
#include "Strahl.hpp"
#include "StrahlComplete.hpp"
#include "UiStyle.hpp"

namespace fs = std::filesystem;

namespace strahlpanel {
namespace {

// What File > New starts from: enough to render something, and to show the
// shape of a scene.
const char* kTemplate =
    "// A new Strahl scene. The reference: docs/strahl.md\n"
    "\n"
    "render { width: 1280, height: 720, samples: 128 }\n"
    "camera { position: [0, 1.5, 5], look_at: [0, 0.5, 0], fov: 40 }\n"
    "sun    { direction: [-1, 2, 1], intensity: 3, angle: 1 }\n"
    "sky    { zenith: #5277b5, horizon: #d8e2ec }\n"
    "\n"
    "plane  { material: matte(checker(#ddd, #555, size: 0.5)) }\n"
    "sphere { center: [0, 0.75, 0], radius: 0.75, material: gold }\n";

// --- Colours for the editor ----------------------------------------------------
// The editor's own lexers know C# and Lua; Strahl's is small enough to live
// here: strings, numbers, #colours, names and punctuation, with // and /* */
// comments handled by the widget itself.
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool tokenize(const char* b, const char* e, const char*& ob, const char*& oe,
              CodeEditor::PaletteIndex& pi) {
    while (b < e && (*b == ' ' || *b == '\t')) ++b;
    if (b == e) {
        ob = oe = e;
        pi = CodeEditor::PaletteIndex::Default;
        return true;
    }
    const char c = *b;
    if (c == '"' || c == '\'') {
        const char* p = b + 1;
        while (p < e && *p != c) { if (*p == '\\' && p + 1 < e) ++p; ++p; }
        ob = b;
        oe = p < e ? p + 1 : e;
        pi = CodeEditor::PaletteIndex::String;
        return true;
    }
    if (c == '#') {
        const char* p = b + 1;
        while (p < e && std::isxdigit(static_cast<unsigned char>(*p))) ++p;
        ob = b;
        oe = p;
        pi = CodeEditor::PaletteIndex::Number;
        return true;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && b + 1 < e && std::isdigit(static_cast<unsigned char>(b[1])))) {
        const char* p = b;
        while (p < e && (std::isdigit(static_cast<unsigned char>(*p)) ||
                         (*p == '.' && !(p + 1 < e && p[1] == '.'))))
            ++p;
        if (p < e && (*p == 'e' || *p == 'E')) {
            const char* q = p + 1;
            if (q < e && (*q == '+' || *q == '-')) ++q;
            if (q < e && std::isdigit(static_cast<unsigned char>(*q))) {
                p = q;
                while (p < e && std::isdigit(static_cast<unsigned char>(*p))) ++p;
            }
        }
        ob = b;
        oe = p;
        pi = CodeEditor::PaletteIndex::Number;
        return true;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        const char* p = b + 1;
        while (p < e && identChar(*p)) ++p;
        ob = b;
        oe = p;
        pi = CodeEditor::PaletteIndex::Identifier;
        return true;
    }
    ob = b;
    oe = b + 1;
    pi = CodeEditor::PaletteIndex::Punctuation;
    return true;
}

const CodeEditor::LanguageDefinition& language() {
    static CodeEditor::LanguageDefinition def;
    static bool inited = false;
    if (!inited) {
        const strahl::Vocabulary& v = strahl::vocabulary();
        for (const char* k : {"let", "fn", "import", "true", "false", "nil", "and", "or", "not"})
            def.mKeywords.insert(k);
        for (const char* k : {"if", "else", "for", "in", "while", "return", "break", "continue"})
            def.mControlKeywords.insert(k);
        for (const std::string& s : v.objects) {
            CodeEditor::Identifier id;
            id.mDeclaration = "object";
            def.mIdentifiers.insert({s, id});
        }
        for (const std::string& s : v.functions) {
            CodeEditor::Identifier id;
            id.mDeclaration = "built in";
            def.mIdentifiers.insert({s, id});
        }
        def.mTokenize = &tokenize;
        def.mCommentStart = "/*";
        def.mCommentEnd = "*/";
        def.mSingleLineComment = "//";
        def.mPreprocChar = '\0';   // # starts a colour
        def.mCaseSensitive = true;
        def.mAutoIndentation = true;
        def.mName = "Strahl";
        inited = true;
    }
    return def;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path libraryDir() {
    std::error_code ec;
    return fs::current_path(ec) / "strahl-lib";
}

std::vector<fs::path> scriptsIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".strahl") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// The editor counts columns in characters, the completion in bytes.
std::size_t byteOfCh(const std::string& line, int ch) {
    std::size_t b = 0;
    for (int c = 0; b < line.size() && c < ch; ++c) {
        ++b;
        while (b < line.size() && (static_cast<unsigned char>(line[b]) & 0xC0) == 0x80) ++b;
    }
    return b;
}

int chOfByte(const std::string& line, std::size_t byte) {
    int ch = 0;
    for (std::size_t b = 0; b < byte && b < line.size(); ++b)
        if ((static_cast<unsigned char>(line[b]) & 0xC0) != 0x80) ++ch;
    return ch;
}

// A letter and a colour per kind of completion, as the C# editor marks them.
struct KindMark { char letter; ImU32 color; };
KindMark kindMark(strahl::complete::Kind k) {
    using K = strahl::complete::Kind;
    const ImU32 purple = IM_COL32(0xB1, 0x80, 0xD7, 255), blue = IM_COL32(0x75, 0xBE, 0xFF, 255),
                orange = IM_COL32(0xEE, 0x9D, 0x28, 255), grey = IM_COL32(0xC0, 0xC0, 0xC0, 255),
                green = IM_COL32(0x89, 0xD1, 0x85, 255);
    switch (k) {
        case K::Keyword:   return {'k', grey};
        case K::Object:    return {'O', orange};
        case K::Property:  return {'P', blue};
        case K::Function:  return {'f', purple};
        case K::Value:     return {'v', blue};
        case K::Parameter: return {'p', green};
        case K::File:      return {'F', grey};
        case K::Snippet:   return {'s', grey};
    }
    return {'.', grey};
}

fs::path nextFree(const fs::path& dir, const std::string& stem, const char* ext) {
    for (int i = 1; i < 10000; ++i) {
        char name[256];
        std::snprintf(name, sizeof name, "%s-%04d.%s", stem.c_str(), i, ext);
        fs::path p = dir / name;
        if (!fs::exists(p)) return p;
    }
    return dir / (stem + "-overflow." + ext);
}

} // namespace

struct State::Impl {
    CodeEditor editor;
    fs::path   file;
    bool       dirty   = false;
    bool       focused = false;
    bool       autoPreview = true;
    double     lastEdit = -1.0;
    std::string builtText;          // what the last build was started from
    fs::path   pendingOpen;         // waiting on "discard changes?"

    // The build, on a worker: a slow script must not stall the editor.
    std::thread       worker;
    std::atomic<bool> building{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    strahl::Result    result;        // the worker's, read once `done`
    bool              wantFull = false;

    // The picture.
    pathtrace::Job    job;
    std::shared_ptr<const pathtrace::Scene> scene;
    pathtrace::Settings settings;   // the script's own
    bool              draft = true;
    fitzel::Texture   preview;
    int    previewW = 0, previewH = 0, previewSamples = -1;
    double previewStamp = 0.0;

    std::vector<strahl::Diagnostic> errors;
    std::vector<std::string>        log;
    std::string stats, saved;

    // Completion (StrahlComplete.hpp): the list under the caret, the
    // signature of the call above it. Worked out again at every keystroke
    // that could change them -- a script is small and the scan is quick.
    struct {
        bool   open = false;
        strahl::complete::Result res;
        int    sel = 0, top = 0;
        ImVec2 rectMin, rectMax;
        bool   hadRect = false;
    } comp;
    bool sigOpen = false;
    strahl::complete::Signature sig;
    bool textFocused = false;   // the text had the keyboard last frame

    Impl() {
        editor.SetLanguageDefinition(language());
        editor.SetPalette(CodeEditor::GetDarkPalette());
        editor.SetTabSize(4);
        editor.SetInsertSpaces(true);
        editor.SetText(kTemplate);
    }
    ~Impl() {
        cancel.store(true);
        if (worker.joinable()) worker.join();
        job.cancel();
    }

    std::string text() const { return editor.GetText(); }

    // --- Completion -----------------------------------------------------------
    // The request for the caret as it stands.
    strahl::complete::Request request(bool manual) const {
        strahl::complete::Request q;
        const std::vector<std::string> lines = editor.GetTextLines();
        const CodeEditor::TextPos at = editor.ToTextPos(editor.GetCursorPosition());
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (static_cast<int>(i) == at.line) q.caret = q.text.size() + byteOfCh(lines[i], at.ch);
            q.text += lines[i];
            if (i + 1 < lines.size()) q.text += '\n';
        }
        q.file = file;
        q.libraryDirs.push_back(libraryDir());
        q.manual = manual;
        return q;
    }

    void closeCompletion() {
        comp.open = false;
        comp.hadRect = false;
        comp.res = {};
    }

    // Ask again. `strong`: open only when the place has something of its own
    // to offer (a sphere's properties after `{ `, materials after
    // `material: `) -- not everything there is, which would be in the way.
    void refreshCompletion(bool manual, bool strong = false) {
        strahl::complete::Result r = strahl::complete::complete(request(manual || strong));
        sigOpen = r.hasSignature;
        if (r.hasSignature) sig = r.signature;
        if (strong && (r.items.empty() || r.items[0].prio < 58 ||
                       r.items[0].kind == strahl::complete::Kind::Parameter)) {
            closeCompletion();
            return;
        }
        // Keep the highlighted item while it is still there.
        const std::string was = comp.open && comp.sel < static_cast<int>(comp.res.items.size())
                                    ? comp.res.items[static_cast<std::size_t>(comp.sel)].label : std::string();
        comp.res = std::move(r);
        comp.open = !comp.res.items.empty();
        comp.sel = 0;
        comp.top = 0;
        if (!was.empty() && comp.res.prefix.empty())
            for (std::size_t i = 0; i < comp.res.items.size(); ++i)
                if (comp.res.items[i].label == was) comp.sel = static_cast<int>(i);
    }

    void updateSignature() {
        const strahl::complete::Result r = strahl::complete::complete(request(false));
        sigOpen = r.hasSignature;
        if (sigOpen) sig = r.signature;
    }

    // Put the highlighted item in place of the word typed so far.
    void commitCompletion() {
        if (!comp.open || comp.sel < 0 || comp.sel >= static_cast<int>(comp.res.items.size())) return;
        const strahl::complete::Item item = comp.res.items[static_cast<std::size_t>(comp.sel)];
        const std::size_t startByte = comp.res.start;
        closeCompletion();
        // The word's start and the caret as {line, ch}: the word never spans lines.
        const std::vector<std::string> lines = editor.GetTextLines();
        const CodeEditor::TextPos at = editor.ToTextPos(editor.GetCursorPosition());
        if (at.line < 0 || at.line >= static_cast<int>(lines.size())) return;
        const std::string& line = lines[static_cast<std::size_t>(at.line)];
        std::size_t lineStart = 0;
        for (int i = 0; i < at.line; ++i) lineStart += lines[static_cast<std::size_t>(i)].size() + 1;
        if (startByte < lineStart) return;
        const int startCh = chOfByte(line, startByte - lineStart);
        editor.ReplaceRange(editor.FromTextPos(at.line, startCh), editor.FromTextPos(at.line, at.ch), item.insert);
        if (item.caret >= 0)
            editor.SetCursorPosition(editor.FromTextPos(at.line, startCh + chOfByte(item.insert, static_cast<std::size_t>(item.caret))));
        dirty = true;
        // What comes next: a call's signature, an import's files, a
        // property's values when it has some of its own.
        using K = strahl::complete::Kind;
        if (item.kind == K::Keyword && item.label == "import") refreshCompletion(false);
        else if (item.kind == K::Property || item.kind == K::Parameter) refreshCompletion(false, true);
        else updateSignature();
    }

    // The list under the word, with the highlighted item's documentation
    // beside it. Drawn over everything, as the C# editor's is.
    void drawCompletion(ImFont* font) {
        comp.hadRect = false;
        if (!comp.open || comp.res.items.empty()) return;
        if (font) ImGui::PushFont(font);
        ImFont* f = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const float rowH = ImGui::GetTextLineHeight() + 4.0f;
        const int n = static_cast<int>(comp.res.items.size());
        const int rows = std::min(n, 10);
        if (comp.sel < comp.top) comp.top = comp.sel;
        if (comp.sel >= comp.top + rows) comp.top = comp.sel - rows + 1;
        comp.top = std::max(0, std::min(comp.top, n - rows));
        float labelW = 220.0f;
        for (int r = 0; r < rows; ++r) {
            const strahl::complete::Item& it = comp.res.items[static_cast<std::size_t>(comp.top + r)];
            labelW = std::max(labelW, ImGui::CalcTextSize(it.label.c_str()).x +
                                          ImGui::CalcTextSize(it.detail.c_str()).x + 24.0f);
        }
        const float width = std::min(labelW + rowH + 24.0f, 640.0f);
        const float h = rows * rowH + 4.0f;
        const CodeEditor::TextPos at = editor.ToTextPos(editor.GetCursorPosition());
        const int startCh = std::max(0, at.ch - chOfByte(comp.res.prefix, comp.res.prefix.size()));
        ImVec2 p0 = editor.CoordinatesToScreen(editor.FromTextPos(at.line, startCh));
        p0.x -= rowH + 4.0f;
        const float lineTop = p0.y;
        p0.y += editor.GetLineHeight();
        const ImVec2 wpos = ImGui::GetMainViewport()->WorkPos, wsz = ImGui::GetMainViewport()->WorkSize;
        if (p0.y + h > wpos.y + wsz.y) p0.y = lineTop - h;   // no room below: above the line
        if (p0.x + width > wpos.x + wsz.x) p0.x = wpos.x + wsz.x - width;
        const ImVec2 p1(p0.x + width, p0.y + h);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(0x25, 0x25, 0x26, 252), 3.0f);
        dl->AddRect(p0, p1, IM_COL32(0x45, 0x45, 0x45, 255), 3.0f);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int clicked = -1;
        const std::string pfx = comp.res.prefix;
        for (int r = 0; r < rows; ++r) {
            const int si = comp.top + r;
            const strahl::complete::Item& it = comp.res.items[static_cast<std::size_t>(si)];
            const ImVec2 r0(p0.x + 2.0f, p0.y + 2.0f + r * rowH), r1(p1.x - 2.0f, r0.y + rowH);
            const bool hot = mouse.x >= r0.x && mouse.x < r1.x && mouse.y >= r0.y && mouse.y < r1.y;
            if (si == comp.sel) dl->AddRectFilled(r0, r1, IM_COL32(0x04, 0x39, 0x5E, 255), 2.0f);
            else if (hot) dl->AddRectFilled(r0, r1, IM_COL32(0x2A, 0x2D, 0x2E, 255), 2.0f);
            if (hot && ImGui::IsMouseClicked(0)) clicked = si;
            const KindMark k = kindMark(it.kind);
            const float ic = rowH - 6.0f;
            const ImVec2 i0(r0.x + 3.0f, r0.y + 3.0f), i1(i0.x + ic, i0.y + ic);
            dl->AddRect(i0, i1, k.color, 2.0f);
            const char letter[2] = {k.letter, 0};
            const ImVec2 ls = ImGui::CalcTextSize(letter);
            dl->AddText(f, fs, ImVec2(i0.x + (ic - ls.x) * 0.5f, i0.y + (ic - ls.y) * 0.5f), k.color, letter);
            const ImVec2 tp(i1.x + 6.0f, r0.y + 2.0f);
            dl->AddText(f, fs, tp, IM_COL32(0xD4, 0xD4, 0xD4, 255), it.label.c_str());
            // The part already typed, brighter.
            std::string ll = it.label, lp = pfx;
            for (char& c : ll) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : lp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!lp.empty() && ll.compare(0, lp.size(), lp) == 0)
                dl->AddText(f, fs, tp, IM_COL32(0x18, 0xA3, 0xFF, 255), it.label.c_str(), it.label.c_str() + lp.size());
            if (!it.detail.empty()) {
                const float w = ImGui::CalcTextSize(it.detail.c_str()).x;
                const float x = std::max(tp.x + ImGui::CalcTextSize(it.label.c_str()).x + 12.0f, r1.x - w - 8.0f);
                dl->PushClipRect(r0, r1, true);
                dl->AddText(f, fs, ImVec2(x, tp.y), IM_COL32(0x8C, 0x8C, 0x8C, 255), it.detail.c_str());
                dl->PopClipRect();
            }
        }
        if (n > rows) {   // where in the list
            const float a = static_cast<float>(comp.top) / n, b = static_cast<float>(comp.top + rows) / n;
            dl->AddRectFilled(ImVec2(p1.x - 4.0f, p0.y + 2.0f + a * (h - 4.0f)),
                              ImVec2(p1.x - 1.0f, p0.y + 2.0f + b * (h - 4.0f)), IM_COL32(0x79, 0x79, 0x79, 160));
        }
        comp.rectMin = p0;
        comp.rectMax = p1;
        const strahl::complete::Item& sel = comp.res.items[static_cast<std::size_t>(comp.sel)];
        if (!sel.doc.empty()) {
            const float dw = 380.0f;
            const ImVec2 ds = f->CalcTextSizeA(fs, FLT_MAX, dw, sel.doc.c_str());
            ImVec2 q0(p1.x + 4.0f, p0.y);
            if (q0.x + dw + 16.0f > wpos.x + wsz.x) q0 = ImVec2(p0.x, p1.y + 4.0f);
            const ImVec2 q1(q0.x + ds.x + 16.0f, q0.y + ds.y + 12.0f);
            dl->AddRectFilled(q0, q1, IM_COL32(0x25, 0x25, 0x26, 252), 3.0f);
            dl->AddRect(q0, q1, IM_COL32(0x45, 0x45, 0x45, 255), 3.0f);
            dl->AddText(f, fs, ImVec2(q0.x + 8.0f, q0.y + 6.0f), IM_COL32(0xD4, 0xD4, 0xD4, 255),
                        sel.doc.c_str(), nullptr, dw);
            comp.rectMax = ImVec2(std::max(p1.x, q1.x), std::max(p1.y, q1.y));
        }
        comp.hadRect = true;
        if (font) ImGui::PopFont();
        if (clicked >= 0) {
            comp.sel = clicked;
            commitCompletion();
        }
    }

    // The call's signature above the caret's line, the parameter being
    // written in it brighter, the function's documentation below it.
    void drawSignature(ImFont* font) {
        if (!sigOpen || sig.label.empty()) return;
        if (font) ImGui::PushFont(font);
        ImFont* f = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const float dw = 520.0f;
        const ImVec2 ls = ImGui::CalcTextSize(sig.label.c_str());
        const ImVec2 ds = sig.doc.empty() ? ImVec2(0, 0) : f->CalcTextSizeA(fs, FLT_MAX, dw, sig.doc.c_str());
        const float w = std::max(ls.x, ds.x) + 16.0f;
        const float h = ls.y + (sig.doc.empty() ? 0.0f : ds.y + 4.0f) + 10.0f;
        const CodeEditor::TextPos at = editor.ToTextPos(editor.GetCursorPosition());
        ImVec2 p0 = editor.CoordinatesToScreen(editor.FromTextPos(at.line, at.ch));
        p0.y -= h + 2.0f;
        p0.x = std::max(p0.x - 40.0f, editor.GetViewMin().x);
        // Above the line, unless the list is up there.
        if (comp.hadRect && comp.rectMax.y > p0.y && comp.rectMin.y < p0.y + h) p0.y = comp.rectMin.y - h - 2.0f;
        const ImVec2 p1(p0.x + w, p0.y + h);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(0x25, 0x25, 0x26, 245), 3.0f);
        dl->AddRect(p0, p1, IM_COL32(0x45, 0x45, 0x45, 255), 3.0f);
        const ImVec2 tp(p0.x + 8.0f, p0.y + 5.0f);
        dl->AddText(f, fs, tp, IM_COL32(0xB0, 0xB0, 0xB0, 255), sig.label.c_str());
        if (sig.active >= 0 && sig.active < static_cast<int>(sig.params.size())) {
            const auto [a, b] = sig.params[static_cast<std::size_t>(sig.active)];
            const char* s = sig.label.c_str();
            const float x = tp.x + ImGui::CalcTextSize(s, s + a).x;
            dl->AddText(f, fs, ImVec2(x, tp.y), IM_COL32(0x18, 0xA3, 0xFF, 255), s + a, s + b);
            const float x2 = tp.x + ImGui::CalcTextSize(s, s + b).x;
            dl->AddLine(ImVec2(x, tp.y + ls.y), ImVec2(x2, tp.y + ls.y), IM_COL32(0x18, 0xA3, 0xFF, 255));
        }
        if (!sig.doc.empty())
            dl->AddText(f, fs, ImVec2(tp.x, tp.y + ls.y + 4.0f), IM_COL32(0x8C, 0x8C, 0x8C, 255),
                        sig.doc.c_str(), nullptr, dw);
        if (font) ImGui::PopFont();
    }

    void stopBuild() {
        cancel.store(true);
        if (worker.joinable()) worker.join();
        cancel.store(false);
        building.store(false);
        done.store(false);
    }

    void startBuild(bool full) {
        stopBuild();
        builtText = text();
        wantFull  = full;
        strahl::Options o;
        o.file = file;
        o.libraryDirs.push_back(libraryDir());
        if (!file.empty()) o.libraryDirs.push_back(file.parent_path());
        building.store(true);
        worker = std::thread([this, o, src = builtText]() mutable {
            o.cancelled = [this] { return cancel.load(); };
            result = strahl::build(src, o);
            done.store(true);
        });
    }

    // The build finished: take its scene, or its errors.
    void collect() {
        if (!done.load()) return;
        if (worker.joinable()) worker.join();
        building.store(false);
        done.store(false);
        log = result.log;
        if (!result.ok) {
            errors = result.errors;
            markErrors();
            return;
        }
        errors.clear();
        editor.SetSquiggles({});
        scene    = result.scene;
        settings = result.settings;
        char line[256];
        std::snprintf(line, sizeof line,
                      "%d shapes, %d meshes (%lld triangles), %d materials, %d lights, "
                      "%d compiled functions -- built in %.2fs",
                      result.shapes, result.meshes, result.triangles, result.materials,
                      result.lights, result.functions, result.seconds);
        stats = line;
        startRender(wantFull);
    }

    // The first error's line underlined from its column to the end of the
    // word there -- the place to look, not the whole line.
    void markErrors() {
        std::vector<CodeEditor::Squiggle> sq;
        const std::string me = file.empty() ? std::string("<script>") : file.string();
        for (const strahl::Diagnostic& d : errors) {
            if (d.line <= 0 || (d.file != me && !(file.empty() && d.file.empty()))) continue;
            const int ln = d.line - 1;
            const std::string row = editor.GetLineText(ln);
            int a = std::max(0, d.col - 1), b = a;
            while (b < static_cast<int>(row.size()) && identChar(row[static_cast<std::size_t>(b)])) ++b;
            if (b == a) b = std::min(a + 1, static_cast<int>(row.size()) + 1);
            CodeEditor::Squiggle s;
            s.start = {ln, a};
            s.end = {ln, b};
            s.severity = 3;
            sq.push_back(s);
        }
        editor.SetSquiggles(sq);
    }

    void startRender(bool full) {
        if (!scene) return;
        pathtrace::Settings s = settings;
        draft = !full;
        if (draft) {
            // A draft: the script's framing at preview size, few samples and
            // bounces -- enough to judge a change, quick enough to follow typing.
            const float aspect = static_cast<float>(s.width) / static_cast<float>(std::max(1, s.height));
            s.width  = 640;
            s.height = std::max(16, static_cast<int>(640.0f / aspect));
            s.samples = std::min(s.samples, 48);
            s.maxBounces = std::min(s.maxBounces, 4);
            s.batch = 2;
        }
        job.start(scene, s);
        previewSamples = -1;
        saved.clear();
    }

    void refresh(double now) {
        if (!job.hasImage()) return;
        const bool running = job.running();
        const int  doneS   = job.samplesDone();
        if (doneS == previewSamples && !running) return;
        if (running && now - previewStamp < 0.3) return;
        std::vector<unsigned char> px;
        if (!job.snapshotLdr(px)) return;
        const int w = job.settings().width, h = job.settings().height;
        if (!preview.isValid() || previewW != w || previewH != h) {
            preview  = fitzel::Texture::blank(w, h);
            previewW = w;
            previewH = h;
        }
        preview.update(px.data(), w, h, 4);
        previewSamples = doneS;
        previewStamp   = now;
    }

    bool save(const fs::path& sceneDir) {
        fs::path target = file;
        const fs::path lib = libraryDir();
        std::error_code ec;
        const bool bundled = !target.empty() &&
                             fs::weakly_canonical(target, ec).string().rfind(
                                 fs::weakly_canonical(lib, ec).string(), 0) == 0;
        if (target.empty() || bundled) {
            // A new script, or a bundled example: saved as the project's own.
            const fs::path dir = sceneDir.empty() ? fs::current_path(ec) : sceneDir / "strahl";
            fs::create_directories(dir, ec);
            const std::string name = target.empty() ? "scene.strahl" : target.filename().string();
            std::string chosen;
            if (ed::saveFile(chosen, dir.string(), name, "Strahl scenes", "*.strahl", "strahl"))
                target = chosen;
            else if (!sceneDir.empty())
                target = nextFree(dir, target.empty() ? "scene" : target.stem().string(), "strahl");
            else
                return false;
        }
        std::ofstream f(target, std::ios::binary);
        if (!f) { saved = "could not write " + target.string(); return false; }
        f << text();
        file  = target;
        dirty = false;
        saved = "saved " + target.string();
        return true;
    }

    void open(const fs::path& p) {
        const std::string src = readFile(p);
        editor.SetText(src);
        file  = p;
        dirty = false;
        errors.clear();
        log.clear();
        editor.SetSquiggles({});
        startBuild(false);
    }

    void saveImage(const fs::path& sceneDir) {
        std::vector<unsigned char> px;
        if (!job.snapshotLdr(px)) return;
        const int w = job.settings().width, h = job.settings().height;
        std::error_code ec;
        const fs::path dir = !sceneDir.empty() ? sceneDir / "renders"
                           : !file.empty()     ? file.parent_path() / "renders"
                                               : fs::current_path(ec) / "renders";
        fs::create_directories(dir, ec);
        const std::string stem = "strahl-" + (file.empty() ? std::string("scene") : file.stem().string());
        const fs::path png = nextFree(dir, stem, "png");
        if (!stbi_write_png(png.string().c_str(), w, h, 4, px.data(), w * 4)) {
            saved = "could not write into " + dir.string();
            return;
        }
        saved = png.string();
        std::vector<float> hdr;
        if (job.snapshotHdr(hdr)) {
            fs::path exr = png;
            exr.replace_extension("exr");
            const char* err = nullptr;
            if (SaveEXR(hdr.data(), w, h, 3, 1, exr.string().c_str(), &err) == TINYEXR_SUCCESS)
                saved += "  (+ .exr)";
            else if (err)
                FreeEXRErrorMessage(err);
        }
    }
};

State::State() : impl(std::make_unique<Impl>()) {}
State::~State() = default;

void State::openFile(const fs::path& f) {
    impl->open(f);
    open = true;
}
std::string State::text() const { return impl->text(); }
const fs::path& State::file() const { return impl->file; }
bool State::focused() const { return impl->focused; }

void draw(State& st, const fs::path& sceneFile, ImFont* mono, double now) {
    State::Impl& m = *st.impl;
    if (!st.open) {
        // Closed: nothing keeps running behind it.
        if (m.job.running()) m.job.cancel();
        if (m.building.load()) m.stopBuild();
        m.focused = false;
        return;
    }
    m.collect();
    // The first time it opens: show what the buffer makes.
    if (!m.scene && !m.building.load() && m.errors.empty() && m.builtText.empty()) m.startBuild(false);
    const fs::path sceneDir = sceneFile.empty() ? fs::path() : sceneFile.parent_path();

    // A size of its own whenever it floats too small to work in. Not
    // FirstUseEver: the area layout (Areas.hpp) creates every panel's window
    // settings on its first frame, so the window is never "first used" and
    // would open as a strip the height of its toolbar.
    if (ImGuiWindow* w = ImGui::FindWindowByName("Strahl###Strahl");
        !w || (!w->DockIsActive && (w->Size.x < 480.0f || w->Size.y < 320.0f)))
        ImGui::SetNextWindowSize(ImVec2(1200, 720), ImGuiCond_Always);
    const std::string title = std::string("Strahl") +
                              (m.file.empty() ? "" : " - " + m.file.filename().string()) +
                              (m.dirty ? " *" : "") + "###Strahl";
    if (!ImGui::Begin(title.c_str(), &st.open)) {
        m.focused = false;
        ImGui::End();
        return;
    }
    m.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // --- The toolbar ------------------------------------------------------
    if (ImGui::Button("New")) {
        if (m.dirty) { m.pendingOpen = fs::path("<new>"); ImGui::OpenPopup("Discard changes?"); }
        else { m.editor.SetText(kTemplate); m.file.clear(); m.dirty = false; m.startBuild(false); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open...")) ImGui::OpenPopup("strahl-open");
    if (ImGui::BeginPopup("strahl-open")) {
        const auto entry = [&](const fs::path& p) {
            if (ImGui::Selectable(p.filename().string().c_str())) {
                if (m.dirty) { m.pendingOpen = p; ImGui::CloseCurrentPopup(); }
                else m.open(p);
            }
        };
        const auto examples = scriptsIn(libraryDir() / "examples");
        if (!examples.empty()) {
            ui::sectionText("Examples");
            for (const fs::path& p : examples) entry(p);
        }
        const auto mine = sceneDir.empty() ? std::vector<fs::path>() : scriptsIn(sceneDir / "strahl");
        if (!mine.empty()) {
            ImGui::Separator();
            ui::sectionText("This project");
            for (const fs::path& p : mine) entry(p);
        }
        ImGui::Separator();
        if (ImGui::Selectable("Browse...")) {
            std::string chosen;
            if (ed::pickFile(chosen, sceneDir.empty() ? std::string() : (sceneDir / "strahl").string(),
                             "Strahl scenes", "*.strahl")) {
                if (m.dirty) m.pendingOpen = chosen;
                else m.open(chosen);
            }
        }
        ImGui::EndPopup();
    }
    if (!m.pendingOpen.empty() && !ImGui::IsPopupOpen("Discard changes?")) ImGui::OpenPopup("Discard changes?");
    if (ImGui::BeginPopupModal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The script has changes that are not saved.");
        if (ImGui::Button("Discard them")) {
            if (m.pendingOpen == fs::path("<new>")) {
                m.editor.SetText(kTemplate);
                m.file.clear();
                m.dirty = false;
                m.startBuild(false);
            } else {
                m.open(m.pendingOpen);
            }
            m.pendingOpen.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing")) { m.pendingOpen.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool saveKey = m.focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
    if (ImGui::Button("Save") || saveKey) m.save(sceneDir);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const bool rendering = m.job.running();
    const bool renderKey = m.focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (ImGui::Button("Render") || renderKey) m.startBuild(true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The script's own render { } settings, in full (Ctrl+Enter)");
    ImGui::SameLine();
    if (ImGui::Button("Preview") || (m.focused && ImGui::IsKeyPressed(ImGuiKey_F5, false))) m.startBuild(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A quick draft of the picture (F5)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!rendering && !m.building.load());
    if (ImGui::Button("Stop")) { m.stopBuild(); m.job.cancel(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Follow typing", &m.autoPreview);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run the script again and redraw the draft a moment after typing stops");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m.job.hasImage());
    if (ImGui::Button("Save image")) m.saveImage(sceneDir);
    ImGui::EndDisabled();

    // --- Editor | picture ---------------------------------------------------
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float leftW = std::max(320.0f, avail.x * 0.5f);
    ImGui::BeginChild("strahl-code", ImVec2(leftW, avail.y), ImGuiChildFlags_ResizeX);
    // Completion's keys, before the text sees them: the list takes the
    // arrows, Tab and Enter (not Ctrl+Enter, which renders) and Esc while it
    // shows; Ctrl+Space asks for it, Ctrl+Shift+Space for the signature.
    ImGuiIO& io = ImGui::GetIO();
    bool suppress = false, accept = false;
    // FITZEL_STRAHL_CARET=<line>:<column> puts the caret there (1-based) a
    // moment after the window opens and asks for completion as Ctrl+Space
    // does: how the list is looked at with no hand on the keyboard (a
    // screenshot of the editor).
    {
        static int caretWait = 0;
        if (caretWait >= 0 && ++caretWait > 20) {
            caretWait = -1;
            int line = 0, col = 0;
            if (const char* c = std::getenv("FITZEL_STRAHL_CARET"); c && std::sscanf(c, "%d:%d", &line, &col) == 2) {
                m.editor.JumpTo(m.editor.FromTextPos(line - 1, col - 1));
                m.refreshCompletion(true);
            }
        }
    }
    if (m.editor.IsFocused()) {
        if (m.comp.open && !m.comp.res.items.empty()) {
            const int n = static_cast<int>(m.comp.res.items.size());
            const int before = m.comp.sel;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m.comp.sel = (m.comp.sel + 1) % n;
            else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m.comp.sel = (m.comp.sel - 1 + n) % n;
            else if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) m.comp.sel = std::min(m.comp.sel + 9, n - 1);
            else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) m.comp.sel = std::max(m.comp.sel - 9, 0);
            else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
                     (!io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))))
                accept = true;
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { m.closeCompletion(); suppress = true; }
            if (m.comp.sel != before || accept) suppress = true;
        } else if (m.sigOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m.sigOpen = false;
            suppress = true;
        }
        if (io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (io.KeyShift) m.updateSignature();
            else m.refreshCompletion(true);
        }
    }
    const bool overList = m.comp.hadRect && ImGui::IsMouseHoveringRect(m.comp.rectMin, m.comp.rectMax, false);
    if (mono) ImGui::PushFont(mono);
    if (suppress) m.editor.SetHandleKeyboardInputs(false);
    if (overList) m.editor.SetHandleMouseInputs(false);
    m.editor.Render("StrahlText");
    m.editor.SetHandleKeyboardInputs(true);
    m.editor.SetHandleMouseInputs(true);
    if (mono) ImGui::PopFont();
    if (m.editor.IsTextChanged()) {
        m.dirty = true;
        m.lastEdit = now;
    }
    if (accept) m.commitCompletion();
    // What the typing asks for.
    const std::string& typed = m.editor.GetTypedChars();
    if (!suppress && !accept) {
        if (!typed.empty()) {
            const char last = typed.back();
            const bool ident = identChar(last);
            if (m.comp.open && !ident && last != '"') m.closeCompletion();
            if (ident || last == '"') {
                // In a word, or opening an import's quotes: the list follows.
                if (m.comp.open || !std::isdigit(static_cast<unsigned char>(last))) m.refreshCompletion(false);
            } else if (last == ':' || last == ' ' || last == '{' || last == ',') {
                // After `material:`, `sphere { `, a property's comma: what
                // belongs there, when the place has something of its own.
                const std::string row = m.editor.GetCurrentLineText();
                const CodeEditor::TextPos at = m.editor.ToTextPos(m.editor.GetCursorPosition());
                std::size_t b = byteOfCh(row, at.ch);
                while (b > 0 && row[b - 1] == ' ') --b;
                const char before = b > 0 ? row[b - 1] : '\0';
                if (last != ' ' || before == ':' || before == '{' || before == ',') m.refreshCompletion(false, true);
                else m.updateSignature();
            } else {
                m.updateSignature();
            }
        } else if (m.editor.WasDeleting()) {
            if (m.comp.open) m.refreshCompletion(false);
            if (m.sigOpen) m.updateSignature();
        } else if (m.editor.IsCursorPositionChanged()) {
            if (m.comp.open) m.closeCompletion();
            if (m.sigOpen) m.updateSignature();
        }
    }
    // Leaving the text for something else lets the popups go. Only on the
    // change: focus asked for (a jump to a line) arrives a frame later.
    if (m.textFocused && !m.editor.IsFocused() && !overList) {
        m.closeCompletion();
        m.sigOpen = false;
    }
    m.textFocused = m.editor.IsFocused();
    m.drawSignature(mono);
    m.drawCompletion(mono);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("strahl-view", ImVec2(0, avail.y));
    m.refresh(now);
    if (m.preview.isValid()) {
        const float w = ImGui::GetContentRegionAvail().x;
        const float scale = w / static_cast<float>(m.previewW);
        ImGui::Image(fitzel::imTexture(m.preview.id()), ImVec2(w, static_cast<float>(m.previewH) * scale));
    } else {
        ImGui::TextDisabled("Press Preview (F5) to see the scene.");
    }
    // Where it stands.
    if (m.building.load()) {
        ImGui::TextUnformatted("running the script...");
    } else if (m.job.hasImage()) {
        const auto& s = m.job.settings();
        ImGui::ProgressBar(m.job.progress(), ImVec2(-1, 0));
        ImGui::Text("%s  %d x %d  %d / %d samples  %.1fs%s", m.draft ? "draft" : "render", s.width, s.height,
                    m.job.samplesDone(), s.samples, m.job.elapsedSeconds(),
                    m.job.running() ? "" : "  (done)");
    }
    if (!m.stats.empty()) ImGui::TextDisabled("%s", m.stats.c_str());
    if (!m.saved.empty()) ImGui::TextDisabled("%s", m.saved.c_str());

    if (!m.errors.empty()) {
        ImGui::Spacing();
        for (const strahl::Diagnostic& d : m.errors) {
            // The file's name, not its path: the place is what matters, and a
            // full path pushes the message itself out of the window.
            strahl::Diagnostic shown = d;
            if (!shown.file.empty() && shown.file[0] != '<')
                shown.file = fs::path(shown.file).filename().string();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", shown.format().c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to go to the line");
            if (ImGui::IsItemClicked() && d.line > 0)
                m.editor.SetCursorPosition(CodeEditor::Coordinates(d.line - 1, std::max(0, d.col - 1)));
        }
        if (m.preview.isValid()) ImGui::TextDisabled("(the picture is the last one that built)");
    }
    if (!m.log.empty()) {
        ImGui::Spacing();
        if (ui::header("Output", ImGuiTreeNodeFlags_DefaultOpen))
            for (const std::string& l : m.log) ImGui::TextWrapped("%s", l.c_str());
    }
    ImGui::EndChild();
    ImGui::End();

    // Following the typing: once it has paused, run what is there now.
    if (m.autoPreview && m.lastEdit >= 0.0 && now - m.lastEdit > 0.6 && !m.building.load()) {
        m.lastEdit = -1.0;
        if (m.text() != m.builtText) m.startBuild(false);
    }
}

} // namespace strahlpanel
