#pragma once

#include <string>
#include <vector>

class TextEditor;

// The Lua script editor's helpers: code completion for what is being typed, and
// the templates a new script starts from. Editor only.
namespace luacomplete {

// One entry in the Lua editor's code-completion list: the identifier to insert
// plus a short signature/description shown greyed after it.
struct Completion { const char* text; const char* hint; };

// The completion popup's state: the matches for the identifier under the cursor
// (the popup shows while this is non-empty and the editor is focused), the word
// being completed, and what Esc last did about it.
struct Completions {
    std::vector<Completion> items;
    std::string             prefix;             // the partial word being completed
    int                     sel  = 0;           // highlighted match
    bool                    open = false;
    bool                    gameMember  = false; // completing after "game."
    bool                    synthMember = false; // completing after "synth."
    bool                    manualClose = false; // Esc: stay closed until
    std::string             closedPrefix;        // the prefix changes
};

// Refresh the candidates from the identifier under the cursor. Called each frame
// after the editor renders, so it sees the latest edit.
void refreshCompletion(TextEditor& ed, Completions& c);

// New-script templates, offered in the "New Script" dialog (printf format: %s is
// the script's name).
extern const char* kTemplateEmpty;
extern const char* kTemplateDocumented;

} // namespace luacomplete
