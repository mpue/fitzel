#pragma once

#include <string>
#include <vector>

struct ImFont;

// The Lua reference, readable inside the editor (Help -> Lua API).
//
// The text is docs/lua-scripting.md -- the one reference there is, copied next
// to the exe by the build -- so the window and the repo can never disagree. It
// is split at its headings into chapters: a list on the left, the chapter on
// the right, and a search box that narrows the list to chapters mentioning the
// word. Everything is a click on a wide row; nothing needs dragging.
namespace luaapi {

struct Section {
    int level = 1;                    // 1 = '#', 2 = '##', ...
    std::string title;
    std::vector<std::string> lines;   // the body, raw markdown
    std::string haystack;             // title + body, lower-case, for search
};

struct State {
    bool open = false;
    bool loaded = false;
    std::string error;                // why the file could not be read
    std::vector<Section> sections;
    int  selected = 0;
    int  shown = -1;                  // chapter last drawn; a new one starts at its top
    char filter[96] = {};
    bool focusFilter = false;         // put the cursor in the search box on open
};

// Open the window (and read the file the first time).
void show(State& s);

// Draw the window if it is open. `mono` is the code font (may be null).
void draw(State& s, ImFont* mono);

} // namespace luaapi
