#include "CodeAssist.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <imgui.h>

using nlohmann::json;
using Coords = CodeEditor::Coordinates;

namespace {

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// How much of the file goes with a request. A local model reads a few
// thousand tokens in a blink; the whole of a long file every pause would not.
constexpr std::size_t kBeforeBytes = 12000;
constexpr std::size_t kAfterBytes  = 4000;
// Lines of context around the region for Ctrl+I.
constexpr int kEditContext = 160;

std::size_t byteOfCh(const std::string& s, int ch) {
    std::size_t b = 0;
    for (int i = 0; i < ch && b < s.size(); ++i) {
        ++b;
        while (b < s.size() && (static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) ++b;
    }
    return b;
}

int chCount(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string rtrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t a = 0;
    for (;;) {
        const std::size_t b = s.find('\n', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

// The text before and after a point, and the rest of its line.
struct Around {
    std::string before, after, rest;
};

Around around(CodeEditor& ed, const Coords& at) {
    Around a;
    const std::vector<std::string> lines = ed.GetTextLines();
    const CodeEditor::TextPos tp = ed.ToTextPos(at);
    if (tp.line < 0 || tp.line >= (int)lines.size()) return a;
    const std::string& cur = lines[tp.line];
    const std::size_t b = byteOfCh(cur, tp.ch);
    for (int l = 0; l < tp.line; ++l) { a.before += lines[l]; a.before += '\n'; }
    a.before += cur.substr(0, b);
    a.rest  = cur.substr(b);
    a.after = a.rest;
    for (int l = tp.line + 1; l < (int)lines.size(); ++l) { a.after += '\n'; a.after += lines[l]; }
    if (a.before.size() > kBeforeBytes) {
        std::size_t cut = a.before.size() - kBeforeBytes;
        const std::size_t nl = a.before.find('\n', cut);
        if (nl != std::string::npos) cut = nl + 1;
        a.before = a.before.substr(cut);
    }
    if (a.after.size() > kAfterBytes) {
        std::size_t cut = a.after.rfind('\n', kAfterBytes);
        a.after = a.after.substr(0, cut == std::string::npos ? kAfterBytes : cut);
    }
    return a;
}

std::string commentLines(const std::string& text, const std::string& language) {
    const std::string mark = language == "lua" ? "-- " : "// ";
    std::string out;
    for (const std::string& l : splitLines(text)) {
        if (l.rfind("--", 0) == 0 || l.rfind("//", 0) == 0) out += l;
        else out += mark + l;
        out += '\n';
    }
    return out;
}

const char* fence(const std::string& language) { return language == "lua" ? "lua" : "csharp"; }
const char* langName(const std::string& language) { return language == "lua" ? "Lua" : "C#"; }

// What the model sends back, cut to what belongs at the caret: up to where it
// starts repeating the text after the caret, one block's worth of lines.
int indentOf(const std::string& l) {
    int n = 0;
    for (char c : l) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

// A line that ends a block: Lua's end / until, a closing brace.
bool closesBlock(const std::string& line) {
    const std::string t = trim(line);
    auto word = [&](const char* w) {
        const std::size_t n = std::strlen(w);
        return t.compare(0, n, w) == 0 && (t.size() == n || !(std::isalnum((unsigned char)t[n]) || t[n] == '_'));
    };
    return word("end") || word("until") || (!t.empty() && t[0] == '}');
}

std::string shapeSuggestion(std::string s, const std::string& before, const std::string& rest,
                            const std::string& after, int maxLines, bool truncated) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    // A special token the server let through ends it.
    for (const char* stop : {"<|", "<fim", "<PRE>", "<SUF>", "<MID>", "<EOT>", "<\xEF\xBD\x9C"}) {
        const std::size_t p = s.find(stop);
        if (p != std::string::npos) s = s.substr(0, p);
    }
    const std::string restTrim = trim(rest);
    if (!restTrim.empty()) {
        // Mid-line (before a `)` or the like): one line, without the closers
        // already there.
        s = s.substr(0, s.find('\n'));
        const std::string r = rtrim(s);
        if (r.size() >= restTrim.size() && r.compare(r.size() - restTrim.size(), restTrim.size(), restTrim) == 0)
            s = r.substr(0, r.size() - restTrim.size());
        return rtrim(s).empty() ? std::string() : s;
    }
    // The first line of what follows: the suggestion stops before it. With its
    // indentation -- an `if`'s own `end` is not the function's.
    std::string next;
    for (const std::string& l : splitLines(after.substr(std::min(after.size(), rest.size())))) {
        next = rtrim(l);
        if (!trim(next).empty()) break;
    }
    const std::vector<std::string> lines = splitLines(s);
    std::vector<std::string> keep;
    int kept = 0;
    bool cut = truncated;   // the model was stopped mid-way: its last lines may be half a block
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        if (i > 0 && t.empty() && kept > 0) { cut = false; break; }            // a blank line: the block is done
        if (i > 0 && !trim(next).empty() && rtrim(lines[i]) == next) { cut = false; break; }   // what is already there
        keep.push_back(lines[i]);
        if (!t.empty()) ++kept;
        if (kept >= maxLines) {
            cut = cut || i + 1 < lines.size();
            break;
        }
    }
    if (cut && keep.size() > 1) {
        // Back to the last whole block: the last line that closes one at the
        // caret's depth or further out. None: the first line alone.
        const std::size_t nl = before.rfind('\n');
        const int caretIndent = indentOf(nl == std::string::npos ? before : before.substr(nl + 1));
        std::size_t last = 0;
        for (std::size_t k = 1; k < keep.size(); ++k)
            if (closesBlock(keep[k]) && indentOf(keep[k]) <= caretIndent) last = k;
        keep.resize(last + 1);
    }
    std::string out;
    for (std::size_t i = 0; i < keep.size(); ++i) {
        if (i > 0) out += '\n';
        out += keep[i];
    }
    out = rtrim(out);
    return trim(out).empty() ? std::string() : out;
}

// A line diff (longest common subsequence) for the Ctrl+I preview: -1 removed,
// +1 added, 0 kept.
std::vector<std::pair<int, std::string>> diffLines(const std::vector<std::string>& a,
                                                   const std::vector<std::string>& b) {
    std::vector<std::pair<int, std::string>> out;
    const std::size_t n = a.size(), m = b.size();
    if (n * m > 400000) {   // too big to be worth it: all of one, all of the other
        for (const auto& l : a) out.push_back({-1, l});
        for (const auto& l : b) out.push_back({1, l});
        return out;
    }
    std::vector<std::vector<int>> L(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = m; j-- > 0;)
            L[i][j] = a[i] == b[j] ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
    std::size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) { out.push_back({0, a[i]}); ++i; ++j; }
        else if (L[i + 1][j] >= L[i][j + 1]) out.push_back({-1, a[i++]});
        else out.push_back({1, b[j++]});
    }
    while (i < n) out.push_back({-1, a[i++]});
    while (j < m) out.push_back({1, b[j++]});
    return out;
}

bool takenKey(ImGuiKey k) { return ImGui::IsKeyPressed(k, false); }

constexpr const char* kRegionMark = "<<<CHANGE HERE>>>";
constexpr const char* kInsertMark = "<<<INSERT HERE>>>";

// The code of an edit answer: its fenced block, and of that only what a model
// that echoed the context put between the markers -- never a marker itself.
std::string editCode(const std::string& answer) {
    std::string s = ollama::firstCodeBlock(ollama::withoutThinking(answer));
    for (const char* mark : {kRegionMark, kInsertMark}) {
        const std::size_t a = s.find(mark);
        if (a == std::string::npos) continue;
        const std::size_t b = s.find(mark, a + 1);
        if (b != std::string::npos) {
            // Between two of them: what was asked for.
            std::size_t from = s.find('\n', a);
            from = from == std::string::npos || from > b ? a + std::strlen(mark) : from + 1;
            s = s.substr(from, b - from);
        } else {
            s.erase(a, std::strlen(mark));
        }
    }
    return rtrim(s);
}

} // namespace

// --- Keys ----------------------------------------------------------------------------

bool CodeAssist::keys(const Doc& d, bool popupOpen) {
    if (!d.ed) return false;
    if (d.ed != m_ed) { reset(); m_ed = d.ed; }
    if (!d.ed->IsFocused()) return false;
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    if (d.ed->HasGhostText() && !popupOpen) {
        if (!ctrl && !shift && !alt && takenKey(ImGuiKey_Tab)) { m_takePending = true; return true; }
        if (ctrl && !shift && !alt && takenKey(ImGuiKey_RightArrow)) {
            m_takePending = true;
            m_takeWord    = true;
            return true;
        }
        if (takenKey(ImGuiKey_Escape)) { clearGhost(); return true; }
    }
    if (ctrl && !shift && !alt && takenKey(ImGuiKey_I)) { startEdit(d, std::string()); return true; }
    // Alt+\ (US), Alt+# (German: the same key).
    if (alt && !ctrl && takenKey(ImGuiKey_Backslash)) {
        m_backoffUntil = 0.0;
        m_manual       = true;
        suggest(d);
        return true;
    }
    return false;
}

// --- After the editor drew ---------------------------------------------------------

bool CodeAssist::after(const Doc& d, bool popupOpen) {
    if (!d.ed) return false;
    if (d.ed != m_ed) { reset(); m_ed = d.ed; }
    CodeEditor& ed = *d.ed;
    ollama::Client& oc = ollama::client();
    oc.poll();   // the model list may be what a suggestion waits for
    bool changed = false;

    // Tab / Ctrl+Right, seen before the editor drew: taken now, so the editor
    // that watches IsTextChanged hears of it through the return value.
    if (m_takePending) {
        m_takePending = false;
        if (ed.HasGhostText()) {
            if (m_takeWord) {
                // Up to the end of the next word (and the spaces before it).
                const std::string& g = m_ghost;
                std::size_t i = 0;
                auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
                while (i < g.size() && !word(g[i]) && g[i] != '\n') ++i;
                while (i < g.size() && word(g[i])) ++i;
                if (i == 0) i = 1;
                const std::string part = g.substr(0, i), rest = g.substr(i);
                ed.ClearGhostText();
                ed.ReplaceRange(m_ghostAt, m_ghostAt, part);
                m_ghost   = rest;
                m_ghostAt = ed.GetCursorPosition();
                if (!m_ghost.empty()) ed.SetGhostText(m_ghostAt, m_ghost);
            } else {
                ed.AcceptGhostText();
                m_ghost.clear();
            }
            changed = true;
        }
        m_takeWord = false;
    }

    // The ghost follows typing that matches it and goes at anything else.
    if (ed.HasGhostText() && !changed) {
        const Coords caret = ed.GetCursorPosition();
        if (ed.IsTextChanged()) {
            const std::string& typed = ed.GetTypedChars();
            const bool along = !typed.empty() && !ed.WasDeleting() && typed.find('\n') == std::string::npos &&
                               m_ghost.size() > typed.size() && m_ghost.compare(0, typed.size(), typed) == 0 &&
                               caret.mLine == m_ghostAt.mLine;
            if (along) {
                m_ghost   = m_ghost.substr(typed.size());
                m_ghostAt = caret;
                ed.SetGhostText(caret, m_ghost);
            } else {
                clearGhost();
            }
        } else if (caret != m_ghostAt || ed.HasSelection()) {
            clearGhost();
        }
    }

    // Typing schedules a suggestion for when it pauses; moving about drops it.
    if (ed.IsTextChanged() && !changed) {
        m_wantSuggest = true;   // typing, Enter, a paste: whatever the editor saw
        m_lastType    = now();
        if (m_req) { oc.cancel(m_req); m_req.reset(); }
    } else if (ed.IsCursorPositionChanged() && !ed.IsTextChanged()) {
        m_wantSuggest = false;
        if (m_req) { oc.cancel(m_req); m_req.reset(); }
    }
    if (popupOpen || ed.HasSelection()) m_wantSuggest = false;
    if (m_wantSuggest && oc.settings.inlineAuto && !ed.HasGhostText() && !m_req && ed.IsFocused() &&
        !m_edit.open && now() - m_lastType > oc.settings.inlineDelay && now() > m_backoffUntil) {
        m_wantSuggest = false;
        m_manual      = false;
        suggest(d);
    }
    if (m_req && m_req->finished()) takeSuggestion(d);

    drawEdit(d, changed);
    return changed;
}

void CodeAssist::clearGhost() {
    if (m_ed) m_ed->ClearGhostText();
    m_ghost.clear();
}

void CodeAssist::reset() {
    ollama::Client& oc = ollama::client();
    if (m_req) oc.cancel(m_req);
    m_req.reset();
    if (m_edit.reply) oc.cancel(m_edit.reply);
    clearGhost();
    m_edit        = Edit();
    m_wantSuggest = false;
    m_takePending = false;
    m_ed          = nullptr;
}

std::string CodeAssist::status() const {
    if (m_edit.reply && !m_edit.reply->finished()) return "AI: writing...";
    if (m_req) return "AI: thinking...";
    // A server that is not there is news when asked, not every time typing pauses.
    if (m_manual && now() < m_backoffUntil && !m_lastError.empty()) return "AI: " + m_lastError;
    return std::string();
}

// --- Inline suggestions ---------------------------------------------------------------

void CodeAssist::suggest(const Doc& d) {
    CodeEditor& ed = *d.ed;
    ollama::Client& oc = ollama::client();
    if (m_req) { oc.cancel(m_req); m_req.reset(); }
    clearGhost();
    const Coords at = ed.GetCursorPosition();
    Around a = around(ed, at);
    // Mid-line only before closers: a suggestion inside a word or before code
    // would have nowhere to go.
    for (char c : a.rest)
        if (!std::isspace(static_cast<unsigned char>(c)) && !std::strchr(")]}\"';,", c)) return;
    const std::string model = oc.codeModel();
    if (model.empty()) {
        if (oc.modelsPending()) {
            // The list is on its way: ask again in a moment.
            m_wantSuggest = true;
            m_lastType    = now();
            return;
        }
        m_lastError    = oc.modelsError().empty() ? "no model" : oc.modelsError();
        m_backoffUntil = now() + 15.0;
        return;
    }
    const ollama::Fim fim = oc.fimFor(model);
    const int maxLines = oc.settings.inlineMaxLines;
    json opts = {{"temperature", 0.15}, {"num_predict", 40 + 28 * maxLines}, {"top_p", 0.9}};
    json body;
    const bool hasRef = !d.reference.empty();
    std::string prompt;
    switch (fim) {
    case ollama::Fim::Qwen:
        // The repository form: the API as a file of its own beside this one.
        prompt = "<|repo_name|>fitzel\n";
        if (hasRef) prompt += "<|file_sep|>api_reference.txt\n" + d.reference + "\n";
        prompt += "<|file_sep|>" + d.path + "\n<|fim_prefix|>" + a.before + "<|fim_suffix|>" + a.after +
                  "<|fim_middle|>";
        opts["stop"] = {"<|endoftext|>", "<|file_sep|>", "<|fim_prefix|>", "<|fim_suffix|>",
                        "<|fim_middle|>", "<|im_end|>", "<|repo_name|>"};
        break;
    case ollama::Fim::CodeLlama:
        prompt = "<PRE> " + (hasRef ? commentLines(d.reference, d.language) : std::string()) + a.before +
                 " <SUF>" + a.after + " <MID>";
        opts["stop"] = {"<EOT>", "<PRE>", "<SUF>", "<MID>"};
        break;
    case ollama::Fim::DeepSeek:
        prompt = "<\xEF\xBD\x9C" "fim\xE2\x96\x81" "begin\xEF\xBD\x9C>" +
                 (hasRef ? commentLines(d.reference, d.language) : std::string()) + a.before +
                 "<\xEF\xBD\x9C" "fim\xE2\x96\x81" "hole\xEF\xBD\x9C>" + a.after +
                 "<\xEF\xBD\x9C" "fim\xE2\x96\x81" "end\xEF\xBD\x9C>";
        break;
    case ollama::Fim::StarCoder:
        prompt = "<fim_prefix>" + (hasRef ? commentLines(d.reference, d.language) : std::string()) + a.before +
                 "<fim_suffix>" + a.after + "<fim_middle>";
        opts["stop"] = {"<|endoftext|>", "<fim_prefix>", "<fim_suffix>", "<fim_middle>"};
        break;
    case ollama::Fim::Suffix:
        body = {{"model", model}, {"prompt", (hasRef ? commentLines(d.reference, d.language) : std::string()) + a.before},
                {"suffix", a.after}, {"stream", false}, {"options", opts}};
        break;
    case ollama::Fim::Chat: {
        // A model that cannot fill a hole: asked in words.
        const std::string sys =
            std::string("You are a code completion engine for ") + langName(d.language) +
            " scripts of the Fitzel game engine. You get a file with the marker <CURSOR>. Reply with "
            "ONLY the code to insert at <CURSOR> -- no explanation, no repeated code, no fences. "
            "At most " + std::to_string(maxLines) + " lines.";
        std::string user;
        if (hasRef) user += "Engine API:\n" + d.reference + "\n\n";
        user += "File " + d.path + ":\n" + a.before + "<CURSOR>" + a.after;
        body = {{"model", model}, {"stream", false}, {"think", false}, {"options", opts},
                {"messages", json::array({{{"role", "system"}, {"content", sys}},
                                          {{"role", "user"}, {"content", user}}})}};
        break;
    }
    }
    if (!prompt.empty())
        body = {{"model", model}, {"prompt", prompt}, {"raw", true}, {"stream", false}, {"options", opts}};
    m_reqAt     = at;
    m_reqBefore = a.before;
    m_reqAfter  = a.after;
    m_reqRest   = a.rest;
    m_req = fim == ollama::Fim::Chat ? oc.chat(body) : oc.generate(body);
}

void CodeAssist::takeSuggestion(const Doc& d) {
    ollama::ReplyPtr r = std::move(m_req);
    m_req.reset();
    CodeEditor& ed = *d.ed;
    std::string text;
    bool truncated = false;
    {
        std::lock_guard<std::mutex> g(r->lock);
        truncated = r->last.is_object() && r->last.value("done_reason", std::string()) == "length";
        if (!r->ok) {
            if (!r->cancelled) {
                m_lastError    = r->error;
                m_backoffUntil = now() + (m_manual ? 5.0 : 30.0);
            }
            return;
        }
        text = r->content;
    }
    m_lastError.clear();
    // Only where it was asked, over the same text.
    if (ed.GetCursorPosition() != m_reqAt || ed.HasSelection() || !ed.IsFocused() || m_edit.open) return;
    const Around a = around(ed, m_reqAt);
    if (a.before != m_reqBefore || a.after != m_reqAfter) return;
    text = ollama::firstCodeBlock(ollama::withoutThinking(text));
    if (!text.empty() && text.back() == '\n' && text.find('\n') + 1 == text.size()) text.pop_back();
    text = shapeSuggestion(text, m_reqBefore, m_reqRest, m_reqAfter, ollama::client().settings.inlineMaxLines,
                           truncated);
    if (text.empty()) return;
    m_ghost   = text;
    m_ghostAt = m_reqAt;
    ed.SetGhostText(m_ghostAt, m_ghost);
}

// --- Ctrl+I --------------------------------------------------------------------------

void CodeAssist::startEdit(const Doc& d, const std::string& instruction) {
    CodeEditor& ed = *d.ed;
    ollama::Client& oc = ollama::client();
    clearGhost();
    if (m_edit.reply) oc.cancel(m_edit.reply);
    m_edit = Edit();
    m_edit.open  = true;
    m_edit.focus = instruction.empty();
    if (ed.HasSelection()) {
        // Whole lines: a model rewrites lines, not the half of one.
        CodeEditor::TextPos a = ed.ToTextPos(ed.GetSelectionStart());
        CodeEditor::TextPos b = ed.ToTextPos(ed.GetSelectionEnd());
        if (b.ch == 0 && b.line > a.line) --b.line;
        m_edit.start = ed.FromTextPos(a.line, 0);
        m_edit.end   = ed.FromTextPos(b.line, 1 << 30);
    } else {
        m_edit.start = m_edit.end = ed.GetCursorPosition();
    }
    std::vector<std::string> lines = ed.GetTextLines();
    const CodeEditor::TextPos a = ed.ToTextPos(m_edit.start), b = ed.ToTextPos(m_edit.end);
    if (a.line != b.line || a.ch != b.ch)
        for (int l = a.line; l <= b.line && l < (int)lines.size(); ++l) {
            if (l > a.line) m_edit.original += '\n';
            m_edit.original += lines[l];
        }
    if (!instruction.empty()) {
        std::snprintf(m_edit.buf, sizeof m_edit.buf, "%s", instruction.c_str());
        sendEdit(d, instruction);
    }
}

void CodeAssist::sendEdit(const Doc& d, const std::string& instruction) {
    CodeEditor& ed = *d.ed;
    ollama::Client& oc = ollama::client();
    if (m_edit.reply) oc.cancel(m_edit.reply);
    m_edit.error.clear();
    const bool insert = m_edit.original.empty();
    if (m_edit.history.empty()) {
        // The file around the region for context, the region marked in it,
        // and the region itself on its own: a model rewrites what it is shown
        // apart far more reliably than a stretch inside a larger text.
        const std::vector<std::string> lines = ed.GetTextLines();
        const CodeEditor::TextPos a = ed.ToTextPos(m_edit.start), b = ed.ToTextPos(m_edit.end);
        const int from = std::max(0, a.line - kEditContext);
        const int to   = std::min((int)lines.size() - 1, b.line + kEditContext);
        std::string file;
        for (int l = from; l < a.line; ++l) file += lines[l] + "\n";
        if (insert) {
            const std::string& cur = lines[a.line];
            const std::size_t at = byteOfCh(cur, a.ch);
            file += cur.substr(0, at) + kInsertMark + cur.substr(at) + "\n";
            for (int l = a.line + 1; l <= to; ++l) file += lines[l] + "\n";
        } else {
            file += std::string(kRegionMark) + "\n";
            for (int l = b.line + 1; l <= to; ++l) file += lines[l] + "\n";
        }
        const std::string lang = fence(d.language);
        const std::string sys =
            std::string("You edit ") + langName(d.language) +
            " scripts of the Fitzel game engine inside its code editor. Reply with ONLY code, in one ```" +
            lang + " fenced block: no explanation before or after it, no markers. Keep the file's "
            "indentation and style. Use only engine API that the reference names or the file already uses.";
        std::string user;
        if (!d.reference.empty()) user += "Engine API reference:\n" + d.reference + "\n\n";
        if (insert) {
            user += "The file " + d.path + " (context; " + kInsertMark + " marks the caret):\n```" + lang + "\n" +
                    file + "```\n\nInstruction: " + instruction +
                    "\n\nReply with only the new code to insert at the caret, in one ```" + lang + " block.";
        } else {
            user += "The file " + d.path + " (context, not to be repeated; " + kRegionMark +
                    " is where the code to change stands):\n```" + lang + "\n" + file +
                    "```\n\nThe code to change:\n```" + lang + "\n" + m_edit.original + "\n```\n\nInstruction: " +
                    instruction + "\n\nReply with the complete new version of the code to change -- all of it, "
                    "changed as instructed, and nothing from around it -- in one ```" + lang + " block.";
        }
        m_edit.history.push_back({{"role", "system"}, {"content", sys}});
        m_edit.history.push_back({{"role", "user"}, {"content", user}});
    } else {
        // Refining: what it wrote last stands, the next instruction follows.
        m_edit.history.push_back({{"role", "assistant"},
                                  {"content", std::string("```") + fence(d.language) + "\n" + m_edit.result + "\n```"}});
        m_edit.history.push_back({{"role", "user"},
                                  {"content", "Change it again: " + instruction +
                                                  "\nReply with the whole new code in one fenced block."}});
    }
    json msgs = json::array();
    for (const json& m : m_edit.history) msgs.push_back(m);
    m_edit.reply      = oc.chat({{"model", oc.codeModel()}, {"messages", msgs}, {"think", false}});
    m_edit.haveResult = false;
}

void CodeAssist::drawEdit(const Doc& d, bool& changed) {
    if (!m_edit.open) return;
    CodeEditor& ed = *d.ed;
    ollama::Client& oc = ollama::client();
    const bool running = m_edit.reply && !m_edit.reply->finished();

    std::string streamed;
    if (m_edit.reply) {
        std::lock_guard<std::mutex> g(m_edit.reply->lock);
        streamed = m_edit.reply->content;
        if (m_edit.reply->done && !m_edit.haveResult) {
            if (m_edit.reply->ok) {
                m_edit.result     = editCode(streamed);
                m_edit.haveResult = true;
                if (trim(m_edit.result).empty()) m_edit.error = "the model wrote no code";
            } else if (!m_edit.reply->cancelled) {
                m_edit.error = m_edit.reply->error;
            }
        }
    }
    std::string shown = m_edit.haveResult ? m_edit.result : editCode(streamed);

    // Below the region's last line, inside the editor's view.
    const ImVec2 vmin = ed.GetViewMin(), vmax = ed.GetViewMax();
    ImVec2 at = ed.CoordinatesToScreen(Coords(m_edit.end.mLine, 0));
    at.y += ed.GetLineHeight() + 2.0f;
    at.x = vmin.x + 40.0f;
    const float width = std::max(360.0f, std::min(760.0f, vmax.x - vmin.x - 60.0f));
    at.y = std::clamp(at.y, vmin.y + 4.0f, std::max(vmin.y + 4.0f, vmax.y - 220.0f));
    ImGui::SetNextWindowPos(at, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    bool keep = true;
    bool accept = false, discard = false, send = false;
    if (ImGui::Begin("##aiedit", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s",
                           m_edit.original.empty() ? "Write code here with AI" : "Change these lines with AI");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", oc.codeModel().empty() ? "no model" : oc.codeModel().c_str());
        if (m_edit.focus) {
            ImGui::SetKeyboardFocusHere();
            m_edit.focus = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const char* hint = m_edit.haveResult ? "Refine: another instruction, Enter"
                                             : "What should it do? Enter sends, Esc closes";
        if (ImGui::InputTextWithHint("##aieditq", hint, m_edit.buf, sizeof m_edit.buf,
                                     ImGuiInputTextFlags_EnterReturnsTrue))
            send = m_edit.buf[0] != '\0';
        const bool inputFocused = ImGui::IsItemFocused();
        const bool here = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if ((here || ed.IsFocused()) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) discard = true;
        if (here && m_edit.haveResult && ImGui::GetIO().KeyCtrl &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            accept = true;
        if (here && m_edit.haveResult && !inputFocused && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) accept = true;

        if (!m_edit.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_edit.error.c_str());
        if (!shown.empty() || running) {
            // The change: what goes red, what comes green.
            const auto diff = diffLines(m_edit.original.empty() ? std::vector<std::string>() : splitLines(m_edit.original),
                                        shown.empty() ? std::vector<std::string>() : splitLines(shown));
            const float lh = ImGui::GetTextLineHeight();
            const float h  = std::min(320.0f, lh * (float)std::max<std::size_t>(diff.size(), 2) + 12.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 1.0f));
            if (ImGui::BeginChild("##aieditdiff", ImVec2(-1.0f, h), true, ImGuiWindowFlags_HorizontalScrollbar)) {
                if (d.mono) ImGui::PushFont(d.mono);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (const auto& [kind, line] : diff) {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float w = ImGui::GetContentRegionAvail().x + ImGui::GetScrollX() + 400.0f;
                    if (kind != 0)
                        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + lh),
                                          kind > 0 ? IM_COL32(40, 90, 50, 150) : IM_COL32(110, 40, 40, 150));
                    const ImVec4 col = kind > 0 ? ImVec4(0.75f, 1.0f, 0.75f, 1.0f)
                                     : kind < 0 ? ImVec4(1.0f, 0.7f, 0.7f, 1.0f)
                                                : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    std::string row = std::string(kind > 0 ? "+ " : kind < 0 ? "- " : "  ");
                    for (char c : line) row += c == '\t' ? std::string(4, ' ') : std::string(1, c);
                    ImGui::TextColored(col, "%s", row.c_str());
                }
                if (running) ImGui::SetScrollHereY(1.0f);
                if (d.mono) ImGui::PopFont();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        if (running) {
            ImGui::TextDisabled("Writing... %d lines", (int)std::count(shown.begin(), shown.end(), '\n') + (shown.empty() ? 0 : 1));
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) oc.cancel(m_edit.reply);
        } else {
            ImGui::BeginDisabled(!m_edit.haveResult || !m_edit.error.empty());
            if (ImGui::Button("Accept")) accept = true;
            ImGui::SetItemTooltip("Ctrl+Enter, or Tab outside the field: one undo step.");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Discard")) discard = true;
            ImGui::SetItemTooltip("Esc");
            if (m_edit.haveResult || !m_edit.error.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Retry") && !m_edit.history.empty()) {
                    // The last question again, without its answer.
                    m_edit.reply = oc.chat({{"model", oc.codeModel()},
                                            {"messages", json(m_edit.history)}, {"think", false}});
                    m_edit.haveResult = false;
                    m_edit.error.clear();
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (send) {
        const std::string q = m_edit.buf;
        sendEdit(d, q);
        m_edit.buf[0] = '\0';
        m_edit.focus  = true;
    }
    if (accept && m_edit.haveResult && m_edit.error.empty()) {
        // Only over the text it was asked about.
        std::string now;
        if (!m_edit.original.empty()) {
            const std::vector<std::string> lines = ed.GetTextLines();
            for (int l = m_edit.start.mLine; l <= m_edit.end.mLine && l < (int)lines.size(); ++l) {
                if (l > m_edit.start.mLine) now += '\n';
                now += lines[l];
            }
        }
        if (now != m_edit.original) {
            m_edit.error = "the code changed under it -- ask again";
        } else {
            std::string text = m_edit.result;
            if (m_edit.original.empty()) {
                // Inserted at the caret: indented like the caret's line.
                const CodeEditor::TextPos p = ed.ToTextPos(m_edit.start);
                const std::string cur = ed.GetLineText(p.line);
                const std::string lead = cur.substr(0, cur.find_first_not_of(" \t") == std::string::npos
                                                           ? cur.size() : cur.find_first_not_of(" \t"));
                const bool atIndent = byteOfCh(cur, p.ch) <= lead.size();
                if (atIndent) {
                    // The model wrote its own indentation: start at the line's start.
                    m_edit.start = ed.FromTextPos(p.line, 0);
                    if (trim(cur).empty()) m_edit.end = ed.FromTextPos(p.line, 1 << 30);
                }
            }
            ed.BeginUndoGroup();
            ed.ReplaceRange(m_edit.start, m_edit.end, text);
            ed.EndUndoGroup();
            changed = true;
            keep    = false;
        }
    }
    if (discard) {
        if (m_edit.reply) oc.cancel(m_edit.reply);
        keep = false;
    }
    if (!keep) m_edit = Edit();
}

// --- Explain, Fix --------------------------------------------------------------------

void CodeAssist::explain(const Doc& d) {
    if (!ask) return;
    CodeEditor& ed = *d.ed;
    std::string code;
    if (ed.HasSelection()) {
        code = ed.GetSelectedText();
    } else {
        const std::vector<std::string> lines = ed.GetTextLines();
        const int c = ed.GetCursorPosition().mLine;
        const int from = lines.size() < 240 ? 0 : std::max(0, c - 40);
        const int to   = lines.size() < 240 ? (int)lines.size() - 1 : std::min((int)lines.size() - 1, c + 40);
        for (int l = from; l <= to; ++l) code += lines[l] + "\n";
    }
    ask("Explain this " + std::string(langName(d.language)) + " code from " + d.path +
        " -- what it does, and anything that looks wrong:\n```" + fence(d.language) + "\n" + rtrim(code) + "\n```");
}

void CodeAssist::fix(const Doc& d) {
    if (d.problems.empty()) return;
    CodeEditor& ed = *d.ed;
    // The problem nearest the caret.
    const int caret = ed.ToTextPos(ed.GetCursorPosition()).line;
    const auto* best = &d.problems.front();
    for (const auto& p : d.problems)
        if (std::abs(p.first - caret) < std::abs(best->first - caret)) best = &p;
    const int total = ed.GetTotalLines();
    const int line  = std::clamp(best->first, 0, std::max(0, total - 1));
    // The lines around it: enough for the model to see the statement whole.
    const int from = std::max(0, line - 10), to = std::min(total - 1, line + 10);
    ed.SetSelection(ed.FromTextPos(from, 0), ed.FromTextPos(to, 1 << 30));
    startEdit(d, "Fix this problem (line " + std::to_string(line + 1) + "): " + best->second);
}

void CodeAssist::menuItems(const Doc& d) {
    ollama::Client& oc = ollama::client();
    const bool can = d.ed != nullptr;
    if (ImGui::MenuItem("Suggest here", "Alt+\\", false, can)) {
        m_backoffUntil = 0.0;
        m_manual       = true;
        if (m_ed != d.ed) { reset(); m_ed = d.ed; }
        suggest(d);
    }
    if (ImGui::MenuItem("Edit with AI...", "Ctrl+I", false, can)) {
        if (m_ed != d.ed) { reset(); m_ed = d.ed; }
        startEdit(d, std::string());
    }
    if (ImGui::MenuItem("Fix problem with AI", nullptr, false, can && !d.problems.empty())) {
        if (m_ed != d.ed) { reset(); m_ed = d.ed; }
        fix(d);
    }
    if (ImGui::MenuItem("Explain in Assistant", nullptr, false, can && (bool)ask)) explain(d);
    ImGui::Separator();
    if (ImGui::MenuItem("Suggest while typing", nullptr, &oc.settings.inlineAuto)) oc.save();
    if (ImGui::MenuItem("AI settings...", nullptr, false, (bool)openSettings)) openSettings();
    if (!m_lastError.empty()) ImGui::TextDisabled("Last error: %s", m_lastError.c_str());
}

std::string CodeAssist::manual(const std::string& name, const std::string& from, const std::string& to) {
    std::ifstream in(name);
    if (!in) in.open("docs/" + name);
    if (!in) return std::string();
    std::string out, line;
    bool inside = from.empty();
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!inside && line.rfind(from, 0) == 0) inside = true;
        else if (inside && !to.empty() && line.rfind(to, 0) == 0) break;
        if (inside) { out += line; out += '\n'; }
    }
    return out;
}
