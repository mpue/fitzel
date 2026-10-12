#include "CSharpEditor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <imgui.h>

#include <fitzel/ui/Gui.hpp>

#include "CSharpProject.hpp"

namespace fs = std::filesystem;
using Coords = CodeEditor::Coordinates;
using json   = nlohmann::json;

namespace {

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Same file? (Windows paths compare without case.)
std::string key(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(path), ec);
    if (ec) p = fs::path(path).lexically_normal();
    std::string s = p.generic_string();
#ifdef _WIN32
    s = lower(s);
#endif
    return s;
}

// Byte offset of character `ch` in a UTF-8 line.
std::size_t byteOf(const std::string& line, int ch) {
    std::size_t b = 0;
    for (int c = 0; c < ch && b < line.size(); ++c) {
        const unsigned char u = static_cast<unsigned char>(line[b]);
        b += u < 0x80 ? 1 : (u >> 5) == 6 ? 2 : (u >> 4) == 14 ? 3 : 4;
    }
    return std::min(b, line.size());
}
int charsIn(const std::string& s, std::size_t from, std::size_t to) {
    int n = 0;
    for (std::size_t b = from; b < to && b < s.size(); ++b)
        if ((static_cast<unsigned char>(s[b]) & 0xC0) != 0x80) ++n;
    return n;
}

CodeEditor::TextPos posOf(const json& j) {
    CodeEditor::TextPos p;
    if (j.is_object()) {
        p.line = j.value("line", 0);
        p.ch   = j.value("ch", 0);
    }
    return p;
}

// Completion kinds (the service's KindOf): a letter and a colour each.
struct KindLook { char letter; ImU32 color; };
KindLook kindLook(int k) {
    const ImU32 purple = IM_COL32(0xB1, 0x80, 0xD7, 255), blue = IM_COL32(0x75, 0xBE, 0xFF, 255),
                orange = IM_COL32(0xEE, 0x9D, 0x28, 255), grey = IM_COL32(0xC0, 0xC0, 0xC0, 255);
    switch (k) {
        case 1: return {'M', purple};   // method
        case 2: return {'P', blue};     // property
        case 3: return {'F', blue};     // field
        case 4: return {'E', orange};   // event
        case 5: return {'v', blue};     // local / parameter
        case 6: return {'C', orange};   // class
        case 7: return {'S', orange};   // struct
        case 8: return {'I', blue};     // interface
        case 9: return {'E', orange};   // enum
        case 10: return {'e', blue};    // enum member
        case 11: return {'K', grey};    // constant
        case 12: return {'D', purple};  // delegate
        case 13: return {'N', grey};    // namespace
        case 14: return {'k', grey};    // keyword
        case 15: return {'s', grey};    // snippet
        case 16: return {'T', orange};  // type parameter
        case 19: return {'O', IM_COL32(0x89, 0xD1, 0x85, 255)};   // scene object (a name in a string)
        default: return {'.', grey};
    }
}

// The service's colour categories (Service.Category) -> the widget's palette.
CodeEditor::PaletteIndex semanticColor(int cat) {
    using P = CodeEditor::PaletteIndex;
    switch (cat) {
        case 1: return P::Type;
        case 2: return P::Method;
        case 3: return P::Member;
        case 4: return P::Variable;
        case 5: return P::EnumMember;
        case 6: return P::Namespace;
        case 7: return P::ControlKeyword;
        case 8: return P::Keyword;
        case 9: return P::ExcludedCode;
        case 10: return P::StringEscape;
        default: return P::Default;
    }
}

// How well `key` matches what was typed: 0 not at all, 4 exactly as typed.
int matchScore(const std::string& key, const std::string& typed) {
    if (typed.empty()) return 1;
    if (key.compare(0, typed.size(), typed) == 0) return 4;
    const std::string lk = lower(key), lt = lower(typed);
    if (lk.compare(0, lt.size(), lt) == 0) return 3;
    // Camel humps: GC -> GetComponent
    {
        std::size_t t = 0;
        for (std::size_t i = 0; i < key.size() && t < typed.size(); ++i) {
            const bool hump = i == 0 || std::isupper(static_cast<unsigned char>(key[i])) || key[i - 1] == '_';
            if (hump && std::tolower(static_cast<unsigned char>(key[i])) == std::tolower(static_cast<unsigned char>(typed[t]))) ++t;
        }
        if (t == typed.size()) return 2;
    }
    // In order somewhere
    std::size_t t = 0;
    for (std::size_t i = 0; i < lk.size() && t < lt.size(); ++i)
        if (lk[i] == lt[t]) ++t;
    return t == lt.size() ? 1 : 0;
}

void wrapText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const std::string& text, float width) {
    dl->AddText(font, size, pos, col, text.c_str(), text.c_str() + text.size(), width);
}
ImVec2 wrapSize(ImFont* font, float size, const std::string& text, float width) {
    return font->CalcTextSizeA(size, FLT_MAX, width, text.c_str(), text.c_str() + text.size());
}

} // namespace

CSharpEditor::CSharpEditor() = default;
CSharpEditor::~CSharpEditor() { m_lang.stop(); }

// --- Files ------------------------------------------------------------------------

CSharpEditor::Doc* CSharpEditor::findDoc(const std::string& path) {
    const std::string k = key(path);
    for (auto& d : m_docs)
        if (key(d->path) == k) return d.get();
    return nullptr;
}

int CSharpEditor::indexOf(const Doc* d) const {
    for (int i = 0; i < (int)m_docs.size(); ++i)
        if (m_docs[i].get() == d) return i;
    return -1;
}

void CSharpEditor::open(const std::string& path, int line, int column) {
    visible = true;
    Doc* d = findDoc(path);
    if (!d) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            m_status   = "Could not open " + path;
            m_statusAt = now();
            return;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            text.erase(0, 3);   // a BOM (Visual Studio writes one)
        auto doc   = std::make_unique<Doc>();
        doc->path  = path;
        doc->title = fs::path(path).filename().string();
        doc->crlf  = text.find("\r\n") != std::string::npos;
        doc->ed.SetText(text);
        doc->ed.SetShowWhitespaces(false);
        doc->ed.SetTabSize(4);
        std::error_code ec;
        doc->diskTime = fs::last_write_time(path, ec);
        m_docs.push_back(std::move(doc));
        d = m_docs.back().get();
    }
    m_selectTab = indexOf(d);
    m_active    = m_selectTab;
    if (line >= 1) jump(*d, line - 1, std::max(0, column - 1));
}

void CSharpEditor::jump(Doc& d, int line, int ch) {
    d.ed.JumpTo(d.ed.FromTextPos(line, ch));
}

bool CSharpEditor::save(Doc& d) {
    std::string text = d.ed.GetText();
    if (d.crlf) {
        std::string out;
        out.reserve(text.size() + text.size() / 16);
        for (char c : text) {
            if (c == '\n') out += '\r';
            out += c;
        }
        text.swap(out);
    }
    std::ofstream out(d.path, std::ios::binary | std::ios::trunc);
    if (!out) {
        m_status   = "Could not write " + d.path;
        m_statusAt = now();
        return false;
    }
    out << text;
    out.close();
    std::error_code ec;
    d.diskTime = fs::last_write_time(d.path, ec);
    d.dirty    = false;
    m_status   = "Saved " + d.title;
    m_statusAt = now();
    return true;
}

int CSharpEditor::saveAll() {
    int n = 0;
    for (auto& d : m_docs)
        if (d->dirty && save(*d)) ++n;
    return n;
}

void CSharpEditor::closeDoc(int index) {
    if (index < 0 || index >= (int)m_docs.size()) return;
    Doc* d = m_docs[index].get();
    if (m_lang.running()) m_lang.request("close", {{"file", d->path}});
    if (m_comp.doc == d) closeCompletion();
    if (m_sig.doc == d) m_sig = {};
    if (m_hover.doc == d) m_hover = {};
    if (m_formatDoc == d) m_formatDoc = nullptr;
    if (m_renameDoc == d) m_renameDoc = nullptr;
    if (m_findDoc == d) m_findDoc = nullptr;
    m_docs.erase(m_docs.begin() + index);
    if (m_active >= (int)m_docs.size()) m_active = (int)m_docs.size() - 1;
    m_selectTab = m_active;
}

void CSharpEditor::edited(Doc& d) {
    ++d.version;
    d.dirty       = true;
    d.changedAt   = now();
    d.needAnalyze = true;
}

json CSharpEditor::at(Doc& d, const Coords& c) {
    const CodeEditor::TextPos p = d.ed.ToTextPos(c);
    return {{"file", d.path}, {"text", d.ed.GetText()}, {"line", p.line}, {"ch", p.ch}};
}

// Edits against the current text ({start, end, text}, not overlapping): applied
// bottom-up, so the positions of those still to come stay right; one undo step.
void CSharpEditor::applyEdits(Doc& d, const json& edits) {
    if (!edits.is_array() || edits.empty()) return;
    std::vector<const json*> list;
    for (const json& e : edits) list.push_back(&e);
    std::sort(list.begin(), list.end(), [](const json* a, const json* b) {
        const auto pa = posOf((*a)["start"]), pb = posOf((*b)["start"]);
        return pa.line != pb.line ? pa.line > pb.line : pa.ch > pb.ch;
    });
    d.ed.BeginUndoGroup();
    for (const json* e : list) {
        const auto s = posOf((*e)["start"]), t = posOf((*e)["end"]);
        d.ed.ReplaceRange(d.ed.FromTextPos(s.line, s.ch), d.ed.FromTextPos(t.line, t.ch), e->value("text", std::string()));
    }
    d.ed.EndUndoGroup();
    edited(d);
}

// --- Language service ---------------------------------------------------------------

void CSharpEditor::requestAnalyze(Doc& d) {
    d.needAnalyze    = false;
    d.analyzeVersion = d.version;
    json a           = {{"file", d.path}, {"text", d.ed.GetText()}};
    d.analyzeId      = m_lang.request("analyze", a, "analyze:" + d.path);
}

void CSharpEditor::requestCompletion(Doc& d, const std::string& trigger) {
    json a = at(d);
    if (!trigger.empty()) a["trigger"] = trigger;
    m_comp.reqId = m_lang.request("complete", a, "complete");
    m_comp.doc   = &d;
}

void CSharpEditor::closeCompletion() {
    m_comp.open = false;
    m_comp.stringMode = false;
    m_comp.reqId = 0;
    m_comp.items.clear();
    m_comp.shown.clear();
    m_comp.desc.clear();
    m_comp.descFor = -1;
    m_comp.hadRect = false;
}

// What has been typed since the list's start decides what shows, best first.
void CSharpEditor::refilter(Doc& d) {
    const CodeEditor::TextPos cur = d.ed.ToTextPos(d.ed.GetCursorPosition());
    if (cur.line != m_comp.start.line || cur.ch < m_comp.start.ch) {
        closeCompletion();
        return;
    }
    const std::string line = d.ed.GetLineText(cur.line);
    const std::size_t b0 = byteOf(line, m_comp.start.ch), b1 = byteOf(line, cur.ch);
    m_comp.prefix = line.substr(b0, b1 - b0);
    for (char c : m_comp.prefix)
        if (m_comp.stringMode ? c == '"' : !isIdent(c)) { closeCompletion(); return; }

    struct Ranked { int i, score; };
    std::vector<Ranked> r;
    for (int i = 0; i < (int)m_comp.items.size(); ++i) {
        const CompItem& it = m_comp.items[i];
        const int s = matchScore(it.filter, m_comp.prefix);
        if (s > 0) r.push_back({i, s});
    }
    std::stable_sort(r.begin(), r.end(), [&](const Ranked& a, const Ranked& b) {
        if (!m_comp.prefix.empty() && a.score != b.score) return a.score > b.score;
        const CompItem &x = m_comp.items[a.i], &y = m_comp.items[b.i];
        if (x.prio != y.prio) return x.prio > y.prio;
        return lower(x.sort) < lower(y.sort);
    });
    const int oldSel = m_comp.sel >= 0 && m_comp.sel < (int)m_comp.shown.size() ? m_comp.shown[m_comp.sel] : -1;
    m_comp.shown.clear();
    for (const Ranked& x : r) m_comp.shown.push_back(x.i);
    m_comp.sel = 0;
    // Keep the highlighted item while it still matches and nothing better came.
    if (m_comp.prefix.empty() && oldSel >= 0)
        for (int i = 0; i < (int)m_comp.shown.size(); ++i)
            if (m_comp.shown[i] == oldSel) { m_comp.sel = i; break; }
    m_comp.top  = 0;
    m_comp.open = !m_comp.shown.empty();
    m_comp.selAt = now();
}

void CSharpEditor::commitCompletion(Doc& d) {
    if (!m_comp.open || m_comp.sel < 0 || m_comp.sel >= (int)m_comp.shown.size()) return;
    const CompItem item = m_comp.items[m_comp.shown[m_comp.sel]];
    const CodeEditor::TextPos start = m_comp.start;
    const bool inString = m_comp.stringMode;
    closeCompletion();
    if (inString) {
        // The string's whole text becomes the name: up to the closing quote.
        const Coords caret = d.ed.GetCursorPosition();
        const auto cur = d.ed.ToTextPos(caret);
        const std::string line = d.ed.GetLineText(cur.line);
        std::size_t b = byteOf(line, cur.ch);
        while (b < line.size() && line[b] != '"') b += line[b] == '\\' && b + 1 < line.size() ? 2 : 1;
        std::string text;
        for (char c : item.label) {
            if (c == '"' || c == '\\') text += '\\';
            text += c;
        }
        d.ed.ReplaceRange(d.ed.FromTextPos(start.line, start.ch),
                          d.ed.FromTextPos(cur.line, charsIn(line, 0, b)), text);
        edited(d);
        return;
    }
    cslang::Reply r;
    json a     = at(d);
    a["index"] = item.index;
    const std::int64_t id = m_lang.request("commit", a, "commit");
    if (id && m_lang.wait(id, 800, r) && r.ok && r.result.contains("edits") && !r.result["edits"].empty()) {
        applyEdits(d, r.result["edits"]);
        if (r.result.contains("caret")) {
            const auto c = posOf(r.result["caret"]);
            const Coords cc = d.ed.FromTextPos(c.line, c.ch);
            d.ed.SetSelection(cc, cc);
            d.ed.SetCursorPosition(cc);
        }
        return;
    }
    // No answer in time: the label in place of what was typed.
    std::string text = item.label;
    const std::size_t lt = text.find('<');
    if (lt != std::string::npos && text.find('>') == text.size() - 1 && lt > 0) text.erase(lt);
    const Coords from = d.ed.FromTextPos(start.line, start.ch);
    d.ed.ReplaceRange(from, d.ed.GetCursorPosition(), text);
    edited(d);
}

void CSharpEditor::requestSignature(Doc& d) {
    m_sig.reqId   = m_lang.request("signature", at(d), "signature");
    m_sig.doc     = &d;
    m_sig.dirty   = false;
    m_sig.lastReq = now();
}

void CSharpEditor::goToDefinition(Doc& d, const Coords& c) {
    m_defId = m_lang.request("definition", at(d, c), "definition");
    m_peek.clear();
    m_peekAt = d.ed.CoordinatesToScreen(c);
    m_peekAt.y += d.ed.GetLineHeight();
}

void CSharpEditor::findReferences(Doc& d) {
    m_refsId = m_lang.request("references", at(d), "references");
    m_bottomOpen = true;
    m_bottomTab  = 1;
    m_refs.clear();
    m_refsName = "...";
}

void CSharpEditor::startRename(Doc& d) {
    Coords a, b;
    if (!d.ed.GetIdentifierAt(d.ed.GetCursorPosition(), a, b)) {
        m_status   = "Nothing to rename here";
        m_statusAt = now();
        return;
    }
    const std::string line = d.ed.GetLineText(a.mLine);
    const auto pa = d.ed.ToTextPos(a), pb = d.ed.ToTextPos(b);
    const std::size_t b0 = byteOf(line, pa.ch), b1 = byteOf(line, pb.ch);
    const std::string word = line.substr(b0, b1 - b0);
    std::snprintf(m_renameBuf, sizeof m_renameBuf, "%s", word.c_str());
    m_renameDoc   = &d;
    m_renameAt    = a;
    m_renameOpen  = true;
    m_renameFocus = true;
    m_renameError.clear();
}

void CSharpEditor::format(Doc& d) {
    m_formatId      = m_lang.request("format", {{"file", d.path}, {"text", d.ed.GetText()}}, "format");
    m_formatDoc     = &d;
    m_formatVersion = d.version;
}

void CSharpEditor::handleReplies() {
    for (cslang::Reply& r : m_lang.poll()) {
        if (!r.ok) {
            if (r.error != "cancelled" && r.op != "text" && r.op != "close")
                std::fprintf(stderr, "[C# editor] %s: %s\n", r.op.c_str(), r.error.c_str());
            if (r.id == m_comp.reqId) m_comp.reqId = 0;
            for (auto& d : m_docs)
                if (r.id == d->analyzeId) { d->analyzeId = 0; d->needAnalyze = true; }
            continue;
        }
        const json& j = r.result;
        // Diagnostics and colours of one file
        for (auto& dp : m_docs) {
            Doc& d = *dp;
            if (r.id != d.analyzeId) continue;
            d.analyzeId = 0;
            if (d.analyzeVersion != d.version) break;   // typed on since: the next one counts
            d.diags.clear();
            std::vector<CodeEditor::Squiggle> sq;
            for (const json& x : j.value("diags", json::array())) {
                Diag g;
                g.start = posOf(x["start"]);
                g.end   = posOf(x["end"]);
                g.sev   = x.value("sev", 3);
                g.code  = x.value("code", std::string());
                g.msg   = x.value("msg", std::string());
                d.diags.push_back(g);
                sq.push_back({g.start, g.end, g.sev});
            }
            d.ed.SetSquiggles(sq);
            std::vector<CodeEditor::SemanticToken> toks;
            const json& t = j.contains("tokens") ? j["tokens"] : json::array();
            for (std::size_t i = 0; i + 3 < t.size(); i += 4)
                toks.push_back({t[i].get<int>(), t[i + 1].get<int>(), t[i + 2].get<int>(), semanticColor(t[i + 3].get<int>())});
            d.ed.SetSemanticTokens(toks);
            break;
        }
        if (r.id == m_comp.reqId) {
            m_comp.reqId = 0;
            Doc* d = m_comp.doc;
            if (!d || d != active()) continue;
            if (j.contains("string")) {
                // A name the editor knows: the scene's objects, each once.
                m_comp.items.clear();
                std::vector<SceneName> names = sceneNames ? sceneNames() : std::vector<SceneName>{};
                std::sort(names.begin(), names.end(), [](const SceneName& a, const SceneName& b) { return a.name < b.name; });
                for (std::size_t i = 0; i < names.size();) {
                    std::size_t k = i;
                    while (k < names.size() && names[k].name == names[i].name) ++k;
                    CompItem it;
                    it.label = it.filter = it.sort = names[i].name;
                    it.inlineDesc = k - i > 1 ? std::to_string(k - i) + " objects" : names[i].kind;
                    it.kind  = 19;
                    it.index = (int)m_comp.items.size();
                    m_comp.items.push_back(std::move(it));
                    i = k;
                }
                m_comp.start      = posOf(j["start"]);
                m_comp.suggestion = false;
                m_comp.stringMode = true;
                m_comp.sel        = 0;
                m_comp.desc.clear();
                m_comp.descFor = -1;
                refilter(*d);
                continue;
            }
            if (!j.contains("items")) continue;
            m_comp.stringMode = false;
            m_comp.items.clear();
            int idx = 0;
            for (const json& x : j["items"]) {
                CompItem it;
                it.label      = x.value("l", std::string());
                it.filter     = x.value("f", it.label);
                it.sort       = x.value("s", it.label);
                it.inlineDesc = x.value("d", std::string());
                it.kind       = x.value("k", 0);
                it.prio       = x.value("p", 0);
                it.index      = idx++;
                m_comp.items.push_back(std::move(it));
            }
            m_comp.start      = posOf(j["start"]);
            m_comp.suggestion = j.value("suggestion", false);
            m_comp.sel        = 0;
            m_comp.desc.clear();
            m_comp.descFor = -1;
            refilter(*d);
        } else if (r.id == m_comp.descId) {
            m_comp.descId = 0;
            m_comp.desc   = j.value("text", std::string());
        } else if (r.id == m_sig.reqId) {
            m_sig.reqId = 0;
            if (j.contains("sigs") && !j["sigs"].empty()) {
                const bool same = m_sig.open && m_sig.data.contains("sigs") && m_sig.data["sigs"].size() == j["sigs"].size();
                m_sig.data = j;
                m_sig.open = true;
                if (!same) m_sig.shownSig = -1;
            } else {
                m_sig.open = false;
            }
        } else if (r.id == m_hover.reqId) {
            m_hover.reqId    = 0;
            m_hover.data     = j;
            m_hover.answered = true;
        } else if (r.id == m_defId) {
            m_defId = 0;
            const json& locs = j.contains("locations") ? j["locations"] : json::array();
            if (!locs.empty()) {
                const json& l = locs[0];
                open(l.value("file", std::string()));
                if (Doc* d = findDoc(l.value("file", std::string()))) jump(*d, l.value("line", 0), l.value("ch", 0));
            } else if (j.contains("metadata")) {
                m_peek = j.value("metadata", std::string()) + "   (" + j.value("assembly", std::string()) + ")";
                if (j.contains("doc") && !j["doc"].get<std::string>().empty()) m_peek += "\n\n" + j["doc"].get<std::string>();
            } else {
                m_status   = "No definition found";
                m_statusAt = now();
            }
        } else if (r.id == m_refsId) {
            m_refsId = 0;
            m_refs.clear();
            m_refsName = j.value("name", std::string());
            for (const json& x : j.value("refs", json::array())) {
                Ref rf;
                rf.file = x.value("file", std::string());
                rf.line = x.value("line", 0);
                rf.ch   = x.value("ch", 0);
                rf.len  = x.value("len", 0);
                rf.text = x.value("text", std::string());
                rf.def  = x.value("def", false);
                m_refs.push_back(std::move(rf));
            }
            std::stable_sort(m_refs.begin(), m_refs.end(), [](const Ref& a, const Ref& b) {
                return a.file != b.file ? a.file < b.file : a.line < b.line;
            });
            if (Doc* d = active()) {
                std::vector<std::pair<Coords, Coords>> hl;
                for (const Ref& rf : m_refs)
                    if (key(rf.file) == key(d->path))
                        hl.push_back({d->ed.FromTextPos(rf.line, rf.ch), d->ed.FromTextPos(rf.line, rf.ch + rf.len)});
                d->ed.SetHighlights(hl);
            }
        } else if (r.id == m_renameId) {
            m_renameId = 0;
            if (j.contains("error")) {
                m_status   = "Rename: " + j["error"].get<std::string>();
                m_statusAt = now();
                continue;
            }
            int files = 0;
            for (const json& f : j.value("files", json::array())) {
                const std::string file = f.value("file", std::string());
                if (Doc* d = findDoc(file)) {
                    applyEdits(*d, f["edits"]);
                } else {
                    std::ofstream out(file, std::ios::binary | std::ios::trunc);
                    if (out) out << f.value("text", std::string());
                }
                ++files;
            }
            m_status   = "Renamed in " + std::to_string(files) + (files == 1 ? " file" : " files");
            m_statusAt = now();
        } else if (r.id == m_formatId) {
            m_formatId = 0;
            if (m_formatDoc && m_formatDoc->version == m_formatVersion && j.contains("edits"))
                applyEdits(*m_formatDoc, j["edits"]);
        }
    }
}

// --- Find --------------------------------------------------------------------------

void CSharpEditor::updateFind(Doc& d) {
    const std::string needle = m_findBuf;
    if (m_findDoc == &d && m_findVersion == d.version && m_findText == needle) return;
    m_findDoc     = &d;
    m_findVersion = d.version;
    m_findText    = needle;
    m_matches.clear();
    if (!needle.empty()) {
        const std::string ln = lower(needle);
        const auto lines = d.ed.GetTextLines();
        for (int l = 0; l < (int)lines.size() && m_matches.size() < 5000; ++l) {
            const std::string hay = lower(lines[l]);
            for (std::size_t p = hay.find(ln); p != std::string::npos; p = hay.find(ln, p + ln.size())) {
                const int c0 = charsIn(lines[l], 0, p), c1 = c0 + charsIn(lines[l], p, p + ln.size());
                m_matches.push_back({d.ed.FromTextPos(l, c0), d.ed.FromTextPos(l, c1)});
            }
        }
    }
    d.ed.SetHighlights(m_matches);
}

void CSharpEditor::findNext(Doc& d, bool backwards) {
    updateFind(d);
    if (m_matches.empty()) return;
    const Coords from = d.ed.HasSelection() ? (backwards ? d.ed.GetSelectionStart() : d.ed.GetSelectionEnd())
                                            : d.ed.GetCursorPosition();
    int pick = -1;
    if (!backwards) {
        for (int i = 0; i < (int)m_matches.size(); ++i)
            if (m_matches[i].first >= from) { pick = i; break; }
        if (pick < 0) pick = 0;
    } else {
        for (int i = (int)m_matches.size() - 1; i >= 0; --i)
            if (m_matches[i].second <= from) { pick = i; break; }
        if (pick < 0) pick = (int)m_matches.size() - 1;
    }
    d.ed.JumpTo(m_matches[pick].second, false);
    d.ed.SetSelection(m_matches[pick].first, m_matches[pick].second);
}

// --- Drawing ------------------------------------------------------------------------

void CSharpEditor::drawEditor(Doc& d, fitzel::Gui& gui, float height) {
    ImGuiIO& io = ImGui::GetIO();
    const bool edFocused = d.ed.IsFocused();
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    bool suppress = false, accept = false;

    // The popups' keys, before the text sees them.
    if (edFocused) {
        if (m_comp.open && m_comp.doc == &d && !m_comp.shown.empty()) {
            const int n = (int)m_comp.shown.size();
            const int before = m_comp.sel;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_comp.sel = (m_comp.sel + 1) % n;
            else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_comp.sel = (m_comp.sel - 1 + n) % n;
            else if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) m_comp.sel = std::min(m_comp.sel + 9, n - 1);
            else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) m_comp.sel = std::max(m_comp.sel - 9, 0);
            else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
                     (!m_comp.suggestion && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))))
                accept = true;
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                closeCompletion();
                suppress = true;
            } else if (!io.InputQueueCharacters.empty() && !(ctrl && !alt) && !m_comp.suggestion &&
                       !m_comp.stringMode && !m_comp.prefix.empty()) {
                // Commit characters: `(` `.` `;` `[` take the highlighted item when it
                // starts with what was typed, then type themselves.
                const ImWchar c = io.InputQueueCharacters[0];
                const CompItem& it = m_comp.items[m_comp.shown[m_comp.sel]];
                if ((c == '(' || c == '.' || c == ';' || c == '[') && matchScore(it.filter, m_comp.prefix) >= 3)
                    commitCompletion(d);
            }
            if (m_comp.sel != before) {
                suppress     = true;
                m_comp.selAt = now();
            }
            if (accept) suppress = true;
        } else if (m_sig.open && m_sig.doc == &d) {
            const int n = m_sig.data.contains("sigs") ? (int)m_sig.data["sigs"].size() : 0;
            const int cur = m_sig.shownSig >= 0 ? m_sig.shownSig : m_sig.data.value("active", 0);
            if (n > 1 && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { m_sig.shownSig = (cur + 1) % n; suppress = true; }
            else if (n > 1 && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { m_sig.shownSig = (cur - 1 + n) % n; suppress = true; }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { m_sig.open = false; suppress = true; }
        }
        if (!m_peek.empty() && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || !io.InputQueueCharacters.empty()))
            m_peek.clear();
        if (ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (shift) requestSignature(d);
            else requestCompletion(d, "");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
            if (shift) findReferences(d);
            else goToDefinition(d, d.ed.GetCursorPosition());
        }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F2, false)) startRename(d);
        if (shift && alt && ImGui::IsKeyPressed(ImGuiKey_F, false)) format(d);
    }

    // A click on the completion list is the list's, not the text's.
    const bool overList = m_comp.open && m_comp.hadRect && m_comp.doc == &d &&
                          ImGui::IsMouseHoveringRect(m_comp.rectMin, m_comp.rectMax, false);
    if (m_mono) ImGui::PushFont(m_mono);
    if (suppress) d.ed.SetHandleKeyboardInputs(false);
    if (overList) d.ed.SetHandleMouseInputs(false);
    d.ed.Render("##code", ImVec2(0.0f, height));
    d.ed.SetHandleKeyboardInputs(true);
    d.ed.SetHandleMouseInputs(true);
    if (m_mono) ImGui::PopFont();
    if (d.ed.IsFocused()) m_focused = true;

    if (d.ed.IsTextChanged()) edited(d);
    if (accept) commitCompletion(d);

    // What the typing asks of the service.
    const std::string& typed   = d.ed.GetTypedChars();
    const bool         deleting = d.ed.WasDeleting();
    if (!suppress && !accept) {
        if (!typed.empty()) {
            const char last = typed.back();
            if (m_comp.open && m_comp.doc == &d) {
                if (isIdent(last) || (m_comp.stringMode && last != '"')) refilter(d);
                else closeCompletion();
            }
            if (!m_comp.open) {
                const auto cur  = d.ed.ToTextPos(d.ed.GetCursorPosition());
                const std::string line = d.ed.GetLineText(cur.line);
                const std::size_t b = byteOf(line, cur.ch);
                const bool wordStart = isIdent(last) && !std::isdigit(static_cast<unsigned char>(last)) &&
                                       (b < 2 || !isIdent(line[b - 2]));
                if (last == '.' || last == ' ' || last == '<' || last == '"' || wordStart)
                    requestCompletion(d, std::string(1, last));
            }
            if (last == '(' || last == ',') requestSignature(d);
            else if (m_sig.open) m_sig.dirty = true;
        } else if (deleting) {
            if (m_comp.open) refilter(d);
            if (m_sig.open) m_sig.dirty = true;
        } else if (d.ed.IsCursorPositionChanged()) {
            if (m_comp.open) closeCompletion();
            if (m_sig.open) m_sig.dirty = true;
        }
    }
    if (m_sig.open && m_sig.dirty && now() - m_sig.lastReq > 0.1) requestSignature(d);
    if (d.needAnalyze && d.analyzeId == 0 && now() - d.changedAt > 0.3 && m_lang.running()) requestAnalyze(d);

    // Hover: quick info and the diagnostics under the mouse; Ctrl: a link.
    const ImVec2 mouse = io.MousePos;
    Coords mc;
    const bool overText = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !overList &&
                          d.ed.ScreenToCoordinates(mouse, mc);
    if (overText) {
        Coords a, b;
        const bool word = d.ed.GetIdentifierAt(mc, a, b);
        if (!word) a = b = mc;
        if (!m_hover.has || m_hover.doc != &d || a != m_hover.a || b != m_hover.b) {
            m_hover           = {};
            m_hover.has       = true;
            m_hover.doc       = &d;
            m_hover.a         = a;
            m_hover.b         = b;
            m_hover.restSince = now();
        }
        const bool rested = now() - m_hover.restSince > 0.35;
        if (word && rested && !m_hover.answered && m_hover.reqId == 0 && m_lang.running())
            m_hover.reqId = m_lang.request("hover", at(d, a), "hover");
        if (ctrl && word) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 p0 = d.ed.CoordinatesToScreen(a), p1 = d.ed.CoordinatesToScreen(b);
            const float y = p0.y + d.ed.GetLineHeight() - 1.0f;
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(0x4E, 0x94, 0xCE, 255), 1.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(0)) goToDefinition(d, a);
        } else if (rested && !ImGui::IsMouseDown(0) && !(m_comp.open && m_comp.doc == &d)) {
            drawHover(d, mc);
        }
    } else {
        m_hover.has = false;
    }

    drawSignature(d);
    drawCompletion(d);

    // A definition outside the project: what it is, until the next key or click.
    if (!m_peek.empty()) {
        if (ImGui::IsMouseClicked(0) || d.ed.IsCursorPositionChanged()) {
            m_peek.clear();
        } else {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImFont* font   = m_mono ? m_mono : ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const ImVec2 sz = wrapSize(font, fs, m_peek, 520.0f);
            const ImVec2 p0 = m_peekAt, p1(p0.x + sz.x + 16.0f, p0.y + sz.y + 12.0f);
            dl->AddRectFilled(p0, p1, IM_COL32(0x25, 0x25, 0x26, 250), 3.0f);
            dl->AddRect(p0, p1, IM_COL32(0x45, 0x45, 0x45, 255), 3.0f);
            wrapText(dl, font, fs, ImVec2(p0.x + 8.0f, p0.y + 6.0f), IM_COL32(0xD4, 0xD4, 0xD4, 255), m_peek, 520.0f);
        }
    }
}

void CSharpEditor::drawCompletion(Doc& d) {
    m_comp.hadRect = false;
    if (!m_comp.open || m_comp.doc != &d || m_comp.shown.empty()) return;
    if (m_mono) ImGui::PushFont(m_mono);
    ImFont* font    = ImGui::GetFont();
    const float fs  = ImGui::GetFontSize();
    const float rowH = ImGui::GetTextLineHeight() + 4.0f;
    const int n     = (int)m_comp.shown.size();
    const int rows  = std::min(n, 10);
    if (m_comp.sel < m_comp.top) m_comp.top = m_comp.sel;
    if (m_comp.sel >= m_comp.top + rows) m_comp.top = m_comp.sel - rows + 1;
    m_comp.top = std::max(0, std::min(m_comp.top, n - rows));

    float labelW = 200.0f;
    for (int r = 0; r < rows; ++r) {
        const CompItem& it = m_comp.items[m_comp.shown[m_comp.top + r]];
        labelW = std::max(labelW, ImGui::CalcTextSize(it.label.c_str()).x +
                                      (it.inlineDesc.empty() ? 0.0f : ImGui::CalcTextSize(it.inlineDesc.c_str()).x + 16.0f));
    }
    const float width = std::min(labelW + rowH + 24.0f, 620.0f);
    const float h     = rows * rowH + 4.0f;
    const Coords startC = d.ed.FromTextPos(m_comp.start.line, m_comp.start.ch);
    ImVec2 p0 = d.ed.CoordinatesToScreen(startC);
    p0.x -= rowH + 4.0f;
    const float lineTop = p0.y;
    p0.y += d.ed.GetLineHeight();
    const ImVec2 vmax = ImGui::GetMainViewport()->WorkPos;
    const ImVec2 vsz  = ImGui::GetMainViewport()->WorkSize;
    if (p0.y + h > vmax.y + vsz.y) p0.y = lineTop - h;   // no room below: above the line
    if (p0.x + width > vmax.x + vsz.x) p0.x = vmax.x + vsz.x - width;
    const ImVec2 p1(p0.x + width, p0.y + h);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0x25, 0x25, 0x26, 252), 3.0f);
    dl->AddRect(p0, p1, IM_COL32(0x45, 0x45, 0x45, 255), 3.0f);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int clicked = -1;
    for (int r = 0; r < rows; ++r) {
        const int si = m_comp.top + r;
        const CompItem& it = m_comp.items[m_comp.shown[si]];
        const ImVec2 r0(p0.x + 2.0f, p0.y + 2.0f + r * rowH), r1(p1.x - 2.0f, r0.y + rowH);
        const bool hot = mouse.x >= r0.x && mouse.x < r1.x && mouse.y >= r0.y && mouse.y < r1.y;
        if (si == m_comp.sel) dl->AddRectFilled(r0, r1, IM_COL32(0x04, 0x39, 0x5E, 255), 2.0f);
        else if (hot) dl->AddRectFilled(r0, r1, IM_COL32(0x2A, 0x2D, 0x2E, 255), 2.0f);
        if (hot && ImGui::IsMouseClicked(0)) clicked = si;
        // kind icon
        const KindLook k = kindLook(it.kind);
        const float ic = rowH - 6.0f;
        const ImVec2 i0(r0.x + 3.0f, r0.y + 3.0f), i1(i0.x + ic, i0.y + ic);
        dl->AddRect(i0, i1, k.color, 2.0f);
        const char letter[2] = {k.letter, 0};
        const ImVec2 ls = ImGui::CalcTextSize(letter);
        dl->AddText(font, fs, ImVec2(i0.x + (ic - ls.x) * 0.5f, i0.y + (ic - ls.y) * 0.5f), k.color, letter);
        // label, the typed part brighter
        const ImVec2 tp(i1.x + 6.0f, r0.y + 2.0f);
        dl->AddText(font, fs, tp, IM_COL32(0xD4, 0xD4, 0xD4, 255), it.label.c_str());
        if (!m_comp.prefix.empty() && lower(it.label).compare(0, m_comp.prefix.size(), lower(m_comp.prefix)) == 0)
            dl->AddText(font, fs, tp, IM_COL32(0x18, 0xA3, 0xFF, 255), it.label.c_str(), it.label.c_str() + m_comp.prefix.size());
        if (!it.inlineDesc.empty()) {
            const float w = ImGui::CalcTextSize(it.inlineDesc.c_str()).x;
            dl->AddText(font, fs, ImVec2(r1.x - w - 6.0f, tp.y), IM_COL32(0x8C, 0x8C, 0x8C, 255), it.inlineDesc.c_str());
        }
    }
    if (n > rows) {   // where in the list
        const float frac0 = (float)m_comp.top / n, frac1 = (float)(m_comp.top + rows) / n;
        dl->AddRectFilled(ImVec2(p1.x - 4.0f, p0.y + 2.0f + frac0 * (h - 4.0f)),
                          ImVec2(p1.x - 1.0f, p0.y + 2.0f + frac1 * (h - 4.0f)), IM_COL32(0x79, 0x79, 0x79, 160));
    }
    m_comp.rectMin = p0;
    m_comp.rectMax = p1;

    // The highlighted item's documentation, asked for once it stays highlighted.
    const CompItem& sel = m_comp.items[m_comp.shown[m_comp.sel]];
    if (!m_comp.stringMode && m_comp.descFor != sel.index && now() - m_comp.selAt > 0.1) {
        m_comp.descFor = sel.index;
        m_comp.desc.clear();
        m_comp.descId = m_lang.request("describe", {{"index", sel.index}}, "describe");
    }
    if (m_comp.descFor == sel.index && !m_comp.desc.empty()) {
        const float dw  = 400.0f;
        const ImVec2 ds = wrapSize(font, fs, m_comp.desc, dw);
        ImVec2 q0(p1.x + 4.0f, p0.y);
        if (q0.x + dw + 16.0f > vmax.x + vsz.x) q0 = ImVec2(p0.x, p1.y + 4.0f);
        const ImVec2 q1(q0.x + ds.x + 16.0f, q0.y + ds.y + 12.0f);
        dl->AddRectFilled(q0, q1, IM_COL32(0x25, 0x25, 0x26, 252), 3.0f);
        dl->AddRect(q0, q1, IM_COL32(0x45, 0x45, 0x45, 255), 3.0f);
        wrapText(dl, font, fs, ImVec2(q0.x + 8.0f, q0.y + 6.0f), IM_COL32(0xD4, 0xD4, 0xD4, 255), m_comp.desc, dw);
        m_comp.rectMax = ImVec2(std::max(p1.x, q1.x), std::max(p1.y, q1.y));
    }
    m_comp.hadRect = true;
    if (m_mono) ImGui::PopFont();
    if (clicked >= 0) {
        m_comp.sel = clicked;
        commitCompletion(d);
    }
}

void CSharpEditor::drawSignature(Doc& d) {
    if (!m_sig.open || m_sig.doc != &d || !m_sig.data.contains("sigs")) return;
    const json& sigs = m_sig.data["sigs"];
    const int n = (int)sigs.size();
    if (n == 0) return;
    int which = m_sig.shownSig >= 0 ? m_sig.shownSig : m_sig.data.value("active", 0);
    which = std::max(0, std::min(which, n - 1));
    const json& sig   = sigs[which];
    const int   param = m_sig.data.value("param", 0);
    const std::string label = sig.value("label", std::string());
    int ps = -1, pe = -1;
    std::string pdoc;
    const json& params = sig.contains("params") ? sig["params"] : json::array();
    if (param < (int)params.size()) {
        ps   = params[param].value("s", -1);
        pe   = params[param].value("e", -1);
        pdoc = params[param].value("doc", std::string());
    } else if (!params.empty() && label.find("params ") != std::string::npos) {
        const json& lastP = params[params.size() - 1];
        ps = lastP.value("s", -1);
        pe = lastP.value("e", -1);
    }
    const std::string sdoc = sig.value("doc", std::string());

    if (m_mono) ImGui::PushFont(m_mono);
    ImFont* font   = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const std::string counter = n > 1 ? std::to_string(which + 1) + "/" + std::to_string(n) + "  " : std::string();
    const float cw = ImGui::CalcTextSize(counter.c_str()).x;
    const float lw = ImGui::CalcTextSize(label.c_str()).x;
    const float width = std::min(std::max(lw + cw, 260.0f), 760.0f);
    std::string body;
    if (!pdoc.empty() && ps >= 0) body = label.substr(ps, pe - ps) + ": " + pdoc;
    if (!sdoc.empty()) body += (body.empty() ? "" : "\n") + sdoc;
    const ImVec2 bs = body.empty() ? ImVec2(0, 0) : wrapSize(font, fs, body, width);
    const float h = ImGui::GetTextLineHeight() + (body.empty() ? 0.0f : bs.y + 6.0f) + 10.0f;

    const Coords caret = d.ed.GetCursorPosition();
    ImVec2 p0 = d.ed.CoordinatesToScreen(caret);
    p0.x -= 20.0f;
    p0.y -= h + 2.0f;
    if (p0.y < d.ed.GetViewMin().y) p0.y = d.ed.CoordinatesToScreen(caret).y + d.ed.GetLineHeight() * 2.0f;
    const ImVec2 p1(p0.x + width + 16.0f, p0.y + h);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0x25, 0x25, 0x26, 252), 3.0f);
    dl->AddRect(p0, p1, IM_COL32(0x45, 0x45, 0x45, 255), 3.0f);
    ImVec2 t(p0.x + 8.0f, p0.y + 5.0f);
    if (!counter.empty()) dl->AddText(font, fs, t, IM_COL32(0x8C, 0x8C, 0x8C, 255), counter.c_str());
    t.x += cw;
    const ImU32 normal = IM_COL32(0xD4, 0xD4, 0xD4, 255), hot = IM_COL32(0x18, 0xA3, 0xFF, 255);
    if (ps >= 0 && pe > ps && pe <= (int)label.size()) {
        const char* s = label.c_str();
        dl->AddText(font, fs, t, normal, s, s + ps);
        const float x1 = t.x + ImGui::CalcTextSize(s, s + ps).x;
        dl->AddText(font, fs, ImVec2(x1, t.y), hot, s + ps, s + pe);
        const float x2 = x1 + ImGui::CalcTextSize(s + ps, s + pe).x;
        dl->AddLine(ImVec2(x1, t.y + ImGui::GetTextLineHeight()), ImVec2(x2, t.y + ImGui::GetTextLineHeight()), hot);
        dl->AddText(font, fs, ImVec2(x2, t.y), normal, s + pe);
    } else {
        dl->AddText(font, fs, t, normal, label.c_str());
    }
    if (!body.empty())
        wrapText(dl, font, fs, ImVec2(p0.x + 8.0f, t.y + ImGui::GetTextLineHeight() + 6.0f), IM_COL32(0xB0, 0xB0, 0xB0, 255),
                 body, width);
    if (m_mono) ImGui::PopFont();
}

void CSharpEditor::drawHover(Doc& d, const Coords& at) {
    const CodeEditor::TextPos p = d.ed.ToTextPos(at);
    std::vector<const Diag*> here;
    for (const Diag& g : d.diags) {
        const bool after  = p.line > g.start.line || (p.line == g.start.line && p.ch >= g.start.ch - (g.start.ch == g.end.ch ? 1 : 0));
        const bool before = p.line < g.end.line || (p.line == g.end.line && p.ch <= g.end.ch);
        if (after && before) here.push_back(&g);
    }
    const json& secs = m_hover.answered && m_hover.data.contains("sections") ? m_hover.data["sections"] : json::array();
    if (here.empty() && secs.empty()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
    for (const Diag* g : here) {
        const ImVec4 col = g->sev >= 3 ? ImVec4(0.95f, 0.45f, 0.45f, 1.0f)
                         : g->sev == 2 ? ImVec4(0.85f, 0.75f, 0.30f, 1.0f) : ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
        ImGui::TextColored(col, "%s", g->code.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(g->msg.c_str());
    }
    if (!here.empty() && !secs.empty()) ImGui::Separator();
    bool first = true;
    for (const json& s : secs) {
        const std::string text = s.value("text", std::string());
        if (first) {
            if (m_mono) ImGui::PushFont(m_mono);
            ImGui::TextUnformatted(text.c_str());
            if (m_mono) ImGui::PopFont();
            first = false;
        } else {
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%s", text.c_str());
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void CSharpEditor::drawBottom(float height) {
    ImGui::BeginChild("##csbottom", ImVec2(0.0f, height), false, ImGuiWindowFlags_NoScrollbar);
    // Problems: the open files' live diagnostics, then the last build's for the others.
    struct Problem { std::string file; int line, ch, sev; std::string text; };
    std::vector<Problem> problems;
    int errors = 0, warnings = 0;
    for (auto& dp : m_docs)
        for (const Diag& g : dp->diags) {
            problems.push_back({dp->path, g.start.line, g.start.ch, g.sev, g.code + ": " + g.msg});
            (g.sev >= 3 ? errors : warnings) += g.sev >= 2 ? 1 : 0;
        }
    if (builder)
        for (const auto& m : builder->messages()) {
            if (findDoc(m.file)) continue;
            problems.push_back({m.file, std::max(0, m.line - 1), 0, m.error ? 3 : 2, m.text});
            (m.error ? errors : warnings)++;
        }

    // Two switches: a click shows that list, a click on the shown one folds it away.
    auto tab = [&](const char* label, int which) {
        const bool on = m_bottomOpen && m_bottomTab == which;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) {
            m_bottomOpen = !on;
            m_bottomTab  = which;
        }
        if (on) ImGui::PopStyleColor();
    };
    char label[64];
    std::snprintf(label, sizeof label, "Problems (%d)###probs", (int)problems.size());
    tab(label, 0);
    ImGui::SameLine();
    std::snprintf(label, sizeof label, "References (%d)###refs", (int)m_refs.size());
    tab(label, 1);
    if (m_bottomOpen) {
        ImGui::BeginChild("##csbottomlist", ImVec2(0.0f, 0.0f), true);
        if (m_bottomTab == 0) {
            if (problems.empty()) ImGui::TextDisabled("No problems.");
            for (std::size_t i = 0; i < problems.size(); ++i) {
                const Problem& p = problems[i];
                ImGui::PushID(static_cast<int>(i));
                const ImVec4 col = p.sev >= 3 ? ImVec4(0.95f, 0.45f, 0.45f, 1.0f) : ImVec4(0.85f, 0.75f, 0.30f, 1.0f);
                ImGui::TextColored(col, p.sev >= 3 ? "error  " : "warning");
                ImGui::SameLine();
                const std::string row = fs::path(p.file).filename().string() + "(" + std::to_string(p.line + 1) + "," +
                                        std::to_string(p.ch + 1) + "): " + p.text;
                if (ImGui::Selectable(row.c_str())) {
                    open(p.file);
                    if (Doc* d = findDoc(p.file)) jump(*d, p.line, p.ch);
                }
                ImGui::PopID();
            }
        } else {
            if (m_refsId) ImGui::TextDisabled("Searching...");
            else if (m_refs.empty()) ImGui::TextDisabled("Shift+F12 on a name lists where it is used.");
            else ImGui::TextDisabled("%d references to %s", (int)m_refs.size(), m_refsName.c_str());
            for (std::size_t i = 0; i < m_refs.size(); ++i) {
                const Ref& r = m_refs[i];
                ImGui::PushID(static_cast<int>(i));
                const std::string row = fs::path(r.file).filename().string() + "(" + std::to_string(r.line + 1) + ")" +
                                        (r.def ? " [definition]  " : "  ") + r.text;
                if (ImGui::Selectable(row.c_str())) {
                    open(r.file);
                    if (Doc* d = findDoc(r.file)) jump(*d, r.line, r.ch);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void CSharpEditor::drawStatus() {
    Doc* d = active();
    if (d) {
        const auto p = d->ed.ToTextPos(d->ed.GetCursorPosition());
        ImGui::Text("Ln %d, Col %d", p.line + 1, p.ch + 1);
        int e = 0, w = 0;
        for (const Diag& g : d->diags) (g.sev >= 3 ? e : w) += g.sev >= 2 ? 1 : 0;
        ImGui::SameLine(0.0f, 24.0f);
        if (e) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%d error%s", e, e == 1 ? "" : "s");
        else ImGui::TextDisabled("0 errors");
        ImGui::SameLine();
        if (w) ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.30f, 1.0f), "%d warning%s", w, w == 1 ? "" : "s");
        else ImGui::TextDisabled("0 warnings");
    }
    if (builder) {
        ImGui::SameLine(0.0f, 24.0f);
        if (builder->running()) ImGui::TextDisabled("Building...");
        else if (builder->generation() > 0 && !builder->ok())
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "Build failed");
        else if (builder->generation() > 0) ImGui::TextDisabled("Built");
    }
    ImGui::SameLine(0.0f, 24.0f);
    if (!m_lang.problem().empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s", m_lang.problem().c_str());
    else if (m_lang.ready()) ImGui::TextDisabled("C# ready");
    else if (m_lang.running()) ImGui::TextDisabled("C# starting...");
    if (!m_status.empty() && now() - m_statusAt < 5.0) {
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextUnformatted(m_status.c_str());
    }
}

void CSharpEditor::drawEmpty() {
    const std::string dir = projectDir ? projectDir() : std::string();
    if (!csproject::isCSharp(dir)) {
        ImGui::TextWrapped("This project has no C# scripts. File > Game Settings sets C# up.");
        return;
    }
    ImGui::TextDisabled("Open a script:");
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::path(dir) / "scripts", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".cs") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
        if (ImGui::Selectable(f.lexically_relative(fs::path(dir) / "scripts").generic_string().c_str()))
            open(f.string());
    ImGui::Spacing();
    if (ImGui::Button("New Script...")) m_newOpen = true;
}

void CSharpEditor::drawPopups() {
    // Rename
    if (m_renameOpen) {
        ImGui::OpenPopup("Rename symbol");
        m_renameOpen = false;
    }
    if (ImGui::BeginPopupModal("Rename symbol", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_renameFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocus = false;
        }
        ImGui::SetNextItemWidth(320.0f);
        const bool enter = ImGui::InputText("New name", m_renameBuf, sizeof m_renameBuf,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if ((enter || ImGui::Button("Rename", ImVec2(110.0f, 0.0f))) && m_renameDoc) {
            json a     = at(*m_renameDoc, m_renameAt);
            a["name"]  = std::string(m_renameBuf);
            m_renameId = m_lang.request("rename", a, "rename");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Go to line
    if (m_gotoOpen) {
        ImGui::OpenPopup("Go to line");
        m_gotoOpen = false;
        m_gotoBuf[0] = '\0';
    }
    if (ImGui::BeginPopupModal("Go to line", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(160.0f);
        const bool enter = ImGui::InputText("Line", m_gotoBuf, sizeof m_gotoBuf,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);
        if (enter || ImGui::Button("Go", ImVec2(110.0f, 0.0f))) {
            if (Doc* d = active()) jump(*d, std::max(0, std::atoi(m_gotoBuf) - 1), 0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // New script
    if (m_newOpen) {
        ImGui::OpenPopup("New C# Script");
        m_newOpen = false;
        m_newBuf[0] = '\0';
    }
    if (ImGui::BeginPopupModal("New C# Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText("Class name", m_newBuf, sizeof m_newBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        std::string id;
        for (const char* c = m_newBuf; *c; ++c)
            if (std::isalnum(static_cast<unsigned char>(*c)) || *c == '_') id += *c;
        if (!id.empty() && std::isdigit(static_cast<unsigned char>(id[0]))) id = "_" + id;
        const std::string dir = projectDir ? projectDir() : std::string();
        const fs::path file   = fs::path(dir) / "scripts" / (id + ".cs");
        std::error_code ec;
        const bool exists = !id.empty() && fs::exists(file, ec);
        if (exists) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "scripts/%s.cs already exists.", id.c_str());
        ImGui::BeginDisabled(id.empty() || exists || dir.empty());
        if (enter || ImGui::Button("Create", ImVec2(110.0f, 0.0f))) {
            if (!id.empty() && !exists && !dir.empty()) {
                fs::create_directories(file.parent_path(), ec);
                std::ofstream out(file, std::ios::binary);
                out << csproject::scriptTemplate(id);
                out.close();
                open(file.string());
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Closing a changed file
    if (m_askClose >= 0 && m_askClose < (int)m_docs.size() && !ImGui::IsPopupOpen("Save changes?"))
        ImGui::OpenPopup("Save changes?");
    if (ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_askClose < 0 || m_askClose >= (int)m_docs.size()) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("%s has changes that are not saved.", m_docs[m_askClose]->title.c_str());
            if (ImGui::Button("Save", ImVec2(110.0f, 0.0f))) {
                if (save(*m_docs[m_askClose])) closeDoc(m_askClose);
                m_askClose = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't save", ImVec2(110.0f, 0.0f))) {
                closeDoc(m_askClose);
                m_askClose = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_askClose = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// --- The window ---------------------------------------------------------------------

void CSharpEditor::panel(fitzel::Gui& gui) {
    m_focused = false;
    m_mono    = gui.monoFont();
    const std::string dir = projectDir ? projectDir() : std::string();
    const bool cs = csproject::isCSharp(dir);
    if (dir != m_project) {
        // Another project: what was open belongs to the old one.
        if (!m_project.empty()) {
            saveAll();
            while (!m_docs.empty()) closeDoc((int)m_docs.size() - 1);
            m_refs.clear();
        }
        m_project = dir;
    }
    if (cs && (visible || !m_docs.empty())) m_lang.ensure(dir);
    else if (!cs && m_lang.running()) m_lang.stop();
    handleReplies();

    // Files changed on disk (Visual Studio, a rename): the unchanged tabs follow.
    if (now() - m_lastDiskCheck > 1.0) {
        m_lastDiskCheck = now();
        for (auto& dp : m_docs) {
            Doc& d = *dp;
            std::error_code ec;
            const auto t = fs::last_write_time(d.path, ec);
            if (ec || t == d.diskTime) continue;
            d.diskTime = t;
            if (d.dirty) continue;
            std::ifstream in(d.path, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const Coords caret = d.ed.GetCursorPosition();
            d.ed.SetText(ss.str());
            d.ed.SetCursorPosition(caret);
            ++d.version;
            d.needAnalyze = true;
            d.changedAt   = 0.0;
        }
    }

    if (!visible) {
        if (m_comp.open) closeCompletion();
        m_sig.open = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(980.0f, 700.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("C# Editor", &visible, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                                             ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    const bool winFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (winFocused) m_focused = true;
    Doc* d = active();
    const bool canEdit = d != nullptr;
    const bool canAsk  = canEdit && m_lang.running();

    bool doSave = false, doClose = false, doFind = false, doGoto = false;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Script...", nullptr, false, cs)) m_newOpen = true;
            if (ImGui::MenuItem("Save", "Ctrl+S", false, canEdit)) doSave = true;
            if (ImGui::MenuItem("Save All", nullptr, false, canEdit)) saveAll();
            if (ImGui::MenuItem("Close", "Ctrl+W", false, canEdit)) doClose = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Open in Visual Studio", nullptr, false, (bool)openInIde))
                openInIde(d ? d->path : std::string());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canEdit && d->ed.CanUndo())) { d->ed.Undo(); edited(*d); }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canEdit && d->ed.CanRedo())) { d->ed.Redo(); edited(*d); }
            ImGui::Separator();
            if (ImGui::MenuItem("Find", "Ctrl+F", false, canEdit)) doFind = true;
            if (ImGui::MenuItem("Go to Line...", "Ctrl+G", false, canEdit)) doGoto = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Toggle Comment", "Ctrl+/", false, canEdit)) { d->ed.ToggleLineComment(); edited(*d); }
            if (ImGui::MenuItem("Format Document", "Shift+Alt+F", false, canAsk)) format(*d);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Go")) {
            if (ImGui::MenuItem("Go to Definition", "F12", false, canAsk)) goToDefinition(*d, d->ed.GetCursorPosition());
            if (ImGui::MenuItem("Find References", "Shift+F12", false, canAsk)) findReferences(*d);
            if (ImGui::MenuItem("Rename Symbol...", "F2", false, canAsk)) startRename(*d);
            ImGui::Separator();
            if (ImGui::MenuItem("Complete", "Ctrl+Space", false, canAsk)) requestCompletion(*d, "");
            if (ImGui::MenuItem("Parameter Info", "Ctrl+Shift+Space", false, canAsk)) requestSignature(*d);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Build")) {
            if (ImGui::MenuItem("Save All and Rebuild", nullptr, false, builder && cs)) {
                saveAll();
                builder->start(dir);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // The window's own keys.
    ImGuiIO& io = ImGui::GetIO();
    if (winFocused && io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) doSave = true;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) doClose = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) doFind = true;
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) doGoto = true;
    }
    if (d && doSave) save(*d);
    if (d && doClose) {
        const int i = indexOf(d);
        if (d->dirty) m_askClose = i;
        else closeDoc(i);
        d = active();
    }
    if (d && doFind) {
        m_findOpen  = true;
        m_findFocus = true;
        if (d->ed.HasSelection()) {
            const std::string s = d->ed.GetSelectedText();
            if (s.find('\n') == std::string::npos) std::snprintf(m_findBuf, sizeof m_findBuf, "%s", s.c_str());
        }
        m_findVersion = -1;
    }
    if (d && doGoto) m_gotoOpen = true;

    if (m_docs.empty()) {
        drawEmpty();
    } else {
        // Tabs
        if (ImGui::BeginTabBar("##cstabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int i = 0; i < (int)m_docs.size(); ++i) {
                Doc& doc  = *m_docs[i];
                bool keep = true;
                ImGuiTabItemFlags f = doc.dirty ? ImGuiTabItemFlags_UnsavedDocument : 0;
                if (m_selectTab == i) f |= ImGuiTabItemFlags_SetSelected;
                const std::string label = doc.title + "###" + doc.path;
                if (ImGui::BeginTabItem(label.c_str(), &keep, f)) {
                    if (m_active != i) {
                        closeCompletion();
                        m_sig.open = false;
                        m_peek.clear();
                    }
                    m_active = i;
                    ImGui::EndTabItem();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", doc.path.c_str());
                if (!keep) {
                    if (doc.dirty) m_askClose = i;
                    else {
                        closeDoc(i);
                        --i;
                    }
                }
            }
            ImGui::EndTabBar();
        }
        m_selectTab = -1;
        d = active();

        // Find bar
        if (d && m_findOpen) {
            ImGui::SetNextItemWidth(280.0f);
            if (m_findFocus) {
                ImGui::SetKeyboardFocusHere();
                m_findFocus = false;
            }
            const bool enter = ImGui::InputTextWithHint("##find", "Find", m_findBuf, sizeof m_findBuf,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            const bool escape = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            updateFind(*d);
            if (enter) {
                findNext(*d, io.KeyShift);
                ImGui::SetKeyboardFocusHere(-1);
            }
            ImGui::SameLine();
            if (m_findBuf[0]) ImGui::TextDisabled("%d match%s", (int)m_matches.size(), m_matches.size() == 1 ? "" : "es");
            ImGui::SameLine();
            if (ImGui::ArrowButton("##findprev", ImGuiDir_Up)) findNext(*d, true);
            ImGui::SameLine();
            if (ImGui::ArrowButton("##findnext", ImGuiDir_Down)) findNext(*d, false);
            ImGui::SameLine();
            if (ImGui::Button("Close##find") || escape) {
                m_findOpen = false;
                m_matches.clear();
                d->ed.SetHighlights({});
            }
        }

        const float statusH = ImGui::GetFrameHeightWithSpacing();
        const float avail   = ImGui::GetContentRegionAvail().y;
        const float bottomH = m_bottomOpen ? std::max(120.0f, avail * 0.24f) : ImGui::GetFrameHeightWithSpacing();
        const float edH     = std::max(80.0f, avail - statusH - bottomH - ImGui::GetStyle().ItemSpacing.y);
        if (d) drawEditor(*d, gui, edH);
        drawBottom(bottomH);
        drawStatus();
    }
    drawPopups();
    ImGui::End();
}
