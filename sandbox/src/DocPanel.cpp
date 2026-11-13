#include "DocPanel.hpp"

#include "UiStyle.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>

namespace docpanel {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Markdown's inline marks read as noise in plain ImGui text: drop the bold
// stars and the code backticks, keep the words.
std::string plain(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') continue;
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') { ++i; continue; }
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') continue;
        out += s[i];
    }
    return out;
}

// "| a | b |" -> {"a", "b"}. An escaped \| stays inside its cell.
std::vector<std::string> cells(const std::string& row) {
    std::vector<std::string> out;
    std::string cur;
    std::string r = trim(row);
    if (!r.empty() && r.front() == '|') r.erase(0, 1);
    if (!r.empty() && r.back() == '|') r.pop_back();
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] == '\\' && i + 1 < r.size() && r[i + 1] == '|') { cur += '|'; ++i; continue; }
        if (r[i] == '|') { out.push_back(plain(trim(cur))); cur.clear(); continue; }
        cur += r[i];
    }
    out.push_back(plain(trim(cur)));
    return out;
}

bool isRuleRow(const std::string& row) {   // |---|:---:|
    for (char c : row)
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    return true;
}

void load(State& s) {
    s.sections.clear();
    s.error.clear();
    // Next to the exe (the build copies it there); the repo copy as a fallback
    // for a run started from the source tree.
    std::ifstream in(s.file);
    if (!in) in.open("docs/" + s.file);
    if (!in) {
        s.error = s.file + " was not found next to the editor. "
                  "Rebuild (build-release.bat) to copy it from docs/.";
        s.loaded = true;
        return;
    }
    std::string line;
    bool inCode = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (startsWith(trim(line), "```")) inCode = !inCode;
        if (!inCode && startsWith(line, "#")) {
            size_t n = 0;
            while (n < line.size() && line[n] == '#') ++n;
            Section sec;
            sec.level = static_cast<int>(n);
            sec.title = plain(trim(line.substr(n)));
            s.sections.push_back(std::move(sec));
            continue;
        }
        if (s.sections.empty()) s.sections.push_back(Section{1, s.title, {}, {}});
        s.sections.back().lines.push_back(line);
    }
    for (Section& sec : s.sections) {
        std::string h = sec.title;
        for (const std::string& l : sec.lines) { h += '\n'; h += l; }
        sec.haystack = lower(h);
    }
    s.selected = std::min(s.selected, std::max(0, static_cast<int>(s.sections.size()) - 1));
    s.loaded = true;
}

bool matches(const Section& sec, const std::string& needle) {
    return needle.empty() || sec.haystack.find(needle) != std::string::npos;
}

bool lineHit(const std::string& line, const std::string& needle) {
    return !needle.empty() && lower(line).find(needle) != std::string::npos;
}

// Where F1 on `word` lands: the first chapter with a row that defines it,
// else the first whose title names it, else the first that mentions it.
// {section, line in it}, line -1 for "its top"; {-1, -1}: nowhere.
std::pair<int, int> definition(const State& s, const std::string& word) {
    const int n = static_cast<int>(s.sections.size());
    const std::string name = word.empty() || word.back() != '(' ? word : word.substr(0, word.size() - 1);
    for (const std::string& w : {word, name})
        for (int i = 0; i < n; ++i)
            for (size_t l = 0; l < s.sections[i].lines.size(); ++l)
                if (defines(s.sections[i].lines[l], w)) return {i, static_cast<int>(l)};
    const std::string needle = lower(name);
    for (int i = 0; i < n; ++i)
        if (lower(s.sections[i].title).find(needle) != std::string::npos) return {i, -1};
    for (int i = 0; i < n; ++i)
        if (s.sections[i].haystack.find(needle) != std::string::npos) return {i, -1};
    return {-1, -1};
}

// The line show(s, word) asked to bring into view, once: drawn, it scrolls.
struct ScrollTo {
    int line = -1;
    void at(int first, int last) {   // a block covering lines [first, last)
        if (line >= first && line < last) { ImGui::SetScrollHereY(0.25f); line = -1; }
    }
};

const ImVec4 kHit(1.0f, 0.78f, 0.35f, 1.0f);

void wrapped(const std::string& text, bool hit) {
    if (hit) ImGui::PushStyleColor(ImGuiCol_Text, kHit);
    ImGui::TextWrapped("%s", text.c_str());
    if (hit) ImGui::PopStyleColor();
}

void drawCode(const std::vector<std::string>& code, int id, ImFont* mono,
              const std::string& needle) {
    std::string all;
    for (const std::string& l : code) { all += l; all += '\n'; }
    ImGui::PushID(id);
    if (mono) ImGui::PushFont(mono, 0.0f);
    const float h = static_cast<float>(code.size()) * ImGui::GetTextLineHeight()
                  + ImGui::GetStyle().WindowPadding.y * 2.0f
                  + ImGui::GetStyle().ScrollbarSize;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("code", ImVec2(0.0f, h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& l : code) {
        const bool hit = lineHit(l, needle);
        if (hit) ImGui::PushStyleColor(ImGuiCol_Text, kHit);
        ImGui::TextUnformatted(l.empty() ? " " : l.c_str());
        if (hit) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (mono) ImGui::PopFont();
    if (ImGui::Button("Copy code")) ImGui::SetClipboardText(all.c_str());
    ImGui::PopID();
    ImGui::Spacing();
}

void drawTable(const std::vector<std::string>& rows, int id, const std::string& needle,
               ScrollTo& scroll) {
    std::vector<std::string> head = cells(rows.front());
    const int cols = std::max(1, static_cast<int>(head.size()));
    // Each column as wide as its text wants, the leading ones capped, so a
    // column of signatures cannot squeeze the descriptions into a word a line.
    std::vector<float> want(static_cast<size_t>(cols), 0.0f);
    for (size_t r = 0; r < rows.size(); ++r) {
        if (isRuleRow(rows[r])) continue;
        const std::vector<std::string> c = cells(rows[r]);
        for (int i = 0; i < cols && i < static_cast<int>(c.size()); ++i)
            want[i] = std::max(want[i], ImGui::CalcTextSize(c[i].c_str()).x);
    }
    const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float cap = avail * (cols <= 2 ? 0.45f : 0.3f);
    float used = 0.0f;
    for (int i = 0; i + 1 < cols; ++i) {
        want[i] = std::clamp(want[i] + ImGui::GetFontSize(), ImGui::GetFontSize() * 3.0f, cap);
        used += want[i];
    }
    want[cols - 1] = std::max(avail - used, avail * 0.3f);
    ImGui::PushID(id);
    if (ImGui::BeginTable("t", cols,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        for (int i = 0; i < cols; ++i)
            ImGui::TableSetupColumn(head[i].c_str(), ImGuiTableColumnFlags_WidthStretch, want[i]);
        ImGui::TableHeadersRow();
        for (size_t r = 1; r < rows.size(); ++r) {
            if (isRuleRow(rows[r])) continue;
            const bool hit = lineHit(rows[r], needle);
            std::vector<std::string> c = cells(rows[r]);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            scroll.at(id + static_cast<int>(r), id + static_cast<int>(r) + 1);
            for (int i = 0; i < cols; ++i) {
                ImGui::TableSetColumnIndex(i);
                wrapped(i < static_cast<int>(c.size()) ? c[i] : std::string(), hit);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    ImGui::Spacing();
}

void drawBody(const Section& sec, ImFont* mono, const std::string& needle, ScrollTo& scroll) {
    // Ids per chapter: two chapters with a table on the same line must not
    // share one table's widths.
    ImGui::PushID(sec.title.c_str());
    const std::vector<std::string>& L = sec.lines;
    for (size_t i = 0; i < L.size();) {
        const std::string t = trim(L[i]);
        if (startsWith(t, "```")) {
            std::vector<std::string> code;
            size_t j = i + 1;
            for (; j < L.size() && !startsWith(trim(L[j]), "```"); ++j) code.push_back(L[j]);
            scroll.at(static_cast<int>(i), static_cast<int>(j + 1));
            drawCode(code, static_cast<int>(i), mono, needle);
            i = j + 1;
        } else if (startsWith(t, "|")) {
            std::vector<std::string> rows;
            size_t j = i;
            for (; j < L.size() && startsWith(trim(L[j]), "|"); ++j) rows.push_back(L[j]);
            drawTable(rows, static_cast<int>(i), needle, scroll);
            i = j;
        } else if (t == "---") {
            ImGui::Separator();
            ++i;
        } else if (t.empty()) {
            ImGui::Spacing();
            ++i;
        } else if (startsWith(t, "- ") || startsWith(t, "* ")) {
            // A bullet runs on over its indented continuation lines.
            std::string item = t.substr(2);
            size_t j = i + 1;
            for (; j < L.size() && startsWith(L[j], "  ") && !trim(L[j]).empty() &&
                   !startsWith(trim(L[j]), "- "); ++j)
                item += " " + trim(L[j]);
            scroll.at(static_cast<int>(i), static_cast<int>(j));
            ImGui::Bullet();
            ImGui::SameLine();
            wrapped(plain(item), lineHit(item, needle));
            i = j;
        } else {
            // A paragraph: consecutive plain lines flow into one.
            std::string para = t;
            size_t j = i + 1;
            for (; j < L.size(); ++j) {
                const std::string n = trim(L[j]);
                if (n.empty() || startsWith(n, "```") || startsWith(n, "|") ||
                    startsWith(n, "- ") || startsWith(n, "* ") || n == "---") break;
                para += " " + n;
            }
            scroll.at(static_cast<int>(i), static_cast<int>(j));
            wrapped(plain(para), lineHit(para, needle));
            i = j;
        }
    }
    ImGui::PopID();
}

} // namespace

void show(State& s) {
    s.open = true;
    s.focusFilter = true;
    s.raise = true;
    load(s);   // re-read each time: after a rebuild the window shows the new text
}

void show(State& s, const std::string& word) {
    show(s);
    const std::string w = trim(word);
    const std::string name = !w.empty() && w.back() == '(' ? w.substr(0, w.size() - 1) : w;
    if (name.empty()) return;
    std::snprintf(s.filter, sizeof(s.filter), "%s", name.c_str());
    s.focusFilter = false;   // the word is the search already; typing over it would lose it
    const auto [sec, line] = definition(s, w);
    if (sec < 0) return;     // the list says that nothing mentions it
    s.selected = sec;
    s.shown = -1;
    s.scrollLine = line;
    s.scrollWait = 3;   // the chapter's layout settles first (tables, wrapping)
}

std::string chapterFor(State& s, const std::string& word) {
    if (!s.loaded) load(s);
    const int sec = definition(s, trim(word)).first;
    return sec < 0 ? std::string() : s.sections[sec].title;
}

void draw(State& s, ImFont* mono) {
    if (!s.open) return;
    if (!s.loaded) load(s);

    // Sized by the font, so it is as roomy at 2x UI scale as at 1x.
    const float em = ImGui::GetFontSize();
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 64.0f, work.x * 0.9f), std::min(em * 44.0f, work.y * 0.85f)),
                             ImGuiCond_FirstUseEver);
    if (s.raise) { ImGui::SetNextWindowFocus(); s.raise = false; }
    if (!ImGui::Begin(s.title.c_str(), &s.open)) { ImGui::End(); return; }

    if (!s.error.empty()) {
        ImGui::TextWrapped("%s", s.error.c_str());
        ImGui::End();
        return;
    }

    // --- Search -------------------------------------------------------------
    if (s.focusFilter) { ImGui::SetKeyboardFocusHere(); s.focusFilter = false; }
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Clear").x
                            - ImGui::GetStyle().FramePadding.x * 2.0f
                            - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##filter", s.searchHint.c_str(), s.filter, sizeof(s.filter));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) s.filter[0] = '\0';
    const std::string needle = lower(trim(s.filter));

    const int n = static_cast<int>(s.sections.size());
    int hits = 0;
    for (const Section& sec : s.sections) hits += matches(sec, needle) ? 1 : 0;
    // A search that hides the open chapter moves to the first one it keeps.
    if (s.selected < n && !matches(s.sections[s.selected], needle))
        for (int i = 0; i < n; ++i)
            if (matches(s.sections[i], needle)) { s.selected = i; break; }

    // --- Chapter list -------------------------------------------------------
    const float listW = std::max(220.0f, ImGui::GetContentRegionAvail().x * 0.28f);
    ImGui::BeginChild("chapters", ImVec2(listW, 0.0f), ImGuiChildFlags_Borders);
    if (!needle.empty()) ui::hint("%d of %d chapters", hits, n);
    for (int i = 0; i < n; ++i) {
        const Section& sec = s.sections[i];
        if (!matches(sec, needle)) continue;
        ImGui::PushID(i);
        const float indent = static_cast<float>(std::max(0, sec.level - 2)) * 12.0f;
        if (indent > 0.0f) ImGui::Indent(indent);
        const bool bold = sec.level <= 2;
        if (bold && ui::boldFont()) ImGui::PushFont(ui::boldFont(), 0.0f);
        if (ImGui::Selectable(sec.title.c_str(), s.selected == i)) s.selected = i;
        if (bold && ui::boldFont()) ImGui::PopFont();
        if (indent > 0.0f) ImGui::Unindent(indent);
        ImGui::PopID();
    }
    ImGui::EndChild();

    // --- Chapter text -------------------------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("text", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (s.selected < n && matches(s.sections[s.selected], needle)) {
        if (s.shown != s.selected) { ImGui::SetScrollY(0.0f); s.shown = s.selected; }
        const Section& sec = s.sections[s.selected];
        if (ui::boldFont()) ImGui::PushFont(ui::boldFont(), ImGui::GetFontSize() * 1.25f);
        ImGui::TextWrapped("%s", sec.title.c_str());
        if (ui::boldFont()) ImGui::PopFont();
        ImGui::Separator();
        ScrollTo scroll{s.scrollWait > 0 ? -1 : s.scrollLine};
        drawBody(sec, mono, needle, scroll);
        if (s.scrollWait > 0) --s.scrollWait;
        else s.scrollLine = scroll.line;
    } else {
        ui::hint("Nothing in the reference mentions \"%s\".", s.filter);
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace docpanel
