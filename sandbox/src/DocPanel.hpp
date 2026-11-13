#pragma once

#include <cctype>
#include <string>
#include <utility>
#include <vector>

struct ImFont;

// A reference, readable inside the editor: Help -> Lua API shows
// docs/lua-scripting.md, Help -> Strahl Reference docs/strahl.md.
//
// The text is the markdown file itself -- the one reference there is, copied
// next to the exe by the build -- so the window and the repo can never
// disagree. It is split at its headings into chapters: a list on the left, the
// chapter on the right, and a search box that narrows the list to chapters
// mentioning the word. Everything is a click on a wide row; nothing needs
// dragging.
namespace docpanel {

struct Section {
    int level = 1;                    // 1 = '#', 2 = '##', ...
    std::string title;
    std::vector<std::string> lines;   // the body, raw markdown
    std::string haystack;             // title + body, lower-case, for search
};

struct State {
    // What it shows: the file beside the exe (docs/<file> from a source-tree
    // run), the window's title, and the search box's example words.
    std::string file;
    std::string title;
    std::string searchHint;

    bool open = false;
    bool loaded = false;
    std::string error;                // why the file could not be read
    std::vector<Section> sections;
    int  selected = 0;
    int  shown = -1;                  // chapter last drawn; a new one starts at its top
    char filter[96] = {};
    bool focusFilter = false;         // put the cursor in the search box on open
    int  scrollLine = -1;             // a line of the chosen chapter to bring into view
    int  scrollWait = 0;              // ...after this many frames of it, once laid out
    bool raise = false;               // to the front (its tab, when docked) next frame

    State(std::string file, std::string title, std::string searchHint)
        : file(std::move(file)), title(std::move(title)), searchHint(std::move(searchHint)) {}
};

// Open the window (and read the file the first time).
void show(State& s);

// Open it on a word, as F1 in a code editor does: the search set to `word`
// and the chapter that DEFINES it chosen -- the one whose table has a row for
// `word(` or `word` -- rather than the first that happens to mention it.
// "word(" (the name stood before a bracket) prefers the row of the call.
void show(State& s, const std::string& word);

// Draw the window if it is open. `mono` is the code font (may be null).
void draw(State& s, ImFont* mono);

// --- What a row defines -----------------------------------------------------------
// Inline, without ImGui: strahlcheck holds docs/strahl.md to the same rule F1
// lands by, so every name the language knows has a row to land on.

// The code spans of a table row's first cell -- `sphere`, `marble(base, ...)`,
// `sun { }` -- which are the names the row is about. Empty for any other line.
inline std::vector<std::string> definedSpans(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] != '|') return out;
    std::size_t end = i + 1;
    while (end < line.size() && !(line[end] == '|' && line[end - 1] != '\\')) ++end;
    const std::string first = line.substr(i + 1, end - i - 1);
    for (std::size_t a = first.find('`'); a != std::string::npos;) {
        const std::size_t b = first.find('`', a + 1);
        if (b == std::string::npos) break;
        out.push_back(first.substr(a + 1, b - a - 1));
        a = first.find('`', b + 1);
    }
    return out;
}

// Does this row define `word`: a span that is the word, or starts with it and
// goes on with something no name has (`word(...)`, `word { }`)? A word ending
// in '(' asks for a call: `min(` finds the function's row, not the property's.
inline bool defines(const std::string& line, const std::string& word) {
    if (word.empty()) return false;
    const bool call = word.back() == '(';
    for (const std::string& span : definedSpans(line)) {
        if (span.rfind(word, 0) != 0) continue;
        if (call || span.size() == word.size()) return true;
        const char c = span[word.size()];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return true;
    }
    return false;
}

// The chapter show(s, word) would choose, by title ("" when the word appears
// nowhere). Loads the file if need be. For tests.
std::string chapterFor(State& s, const std::string& word);

} // namespace docpanel
