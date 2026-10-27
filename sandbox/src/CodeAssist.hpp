#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CodeEditor.hpp"
#include "Ollama.hpp"

struct ImFont;

// A language model's help in a code editor -- the Lua one (ScriptEditor.hpp)
// and the C# one (CSharpEditor.hpp) alike, both being CodeEditors:
//
//   Suggestions as you type: when typing pauses, the model is asked what comes
//   next -- the text before the caret and after it, fill-in-the-middle -- and
//   its answer shows greyed at the caret. Tab takes it, Ctrl+Right takes a
//   word of it, Esc or typing something else lets it go; typing what it says
//   walks along it. Alt+\ (Alt+# on a German keyboard) asks without waiting.
//
//   Ctrl+I: an instruction for the selected lines (or for the caret: new
//   code there). The answer is shown as a change beside the text before
//   anything happens; Accept makes it one undo step, and a further
//   instruction refines it.
//
//   Fix with AI, Explain: the problem nearest the caret goes the Ctrl+I way
//   with "fix this" as the instruction; the selection (or the code around the
//   caret) goes to the Assistant window as a question.
//
// The model learns the engine's API from Doc::reference -- for Lua that is
// luacomplete::apiSummary(), what completion offers. One CodeAssist per editor
// window; it follows whichever CodeEditor it is handed. Editor only.
class CodeAssist {
public:
    struct Doc {
        CodeEditor* ed = nullptr;
        std::string language;    // "lua" | "csharp"
        std::string path;        // as the model sees it: "scripts/door.lua"
        std::string reference;   // the API the model cannot know ("" = none)
        // The file's problems: line (0-based), message. For Fix.
        std::vector<std::pair<int, std::string>> problems;
        ImFont*     mono = nullptr;
    };
    // The Assistant window: a question about the code, asked there.
    std::function<void(const std::string& prompt)> ask;
    // The Assistant window's settings (models, server).
    std::function<void()> openSettings;

    // Before the editor renders: the keys this frame belongs to the assist.
    // True when it took one -- the editor must not see the keyboard then.
    // `popupOpen`: the editor's own completion list is showing (it keeps Tab).
    bool keys(const Doc& d, bool popupOpen);
    // After the editor renders: what the typing asks of the model, the answers
    // that came, the Ctrl+I box. True when it changed the text (a suggestion
    // taken, an edit accepted) -- the editor marks the file changed then.
    bool after(const Doc& d, bool popupOpen);
    // The AI menu's items (inside the editor's BeginMenu("AI")).
    void menuItems(const Doc& d);
    // A word for the status line: what the model is doing ("" = nothing).
    std::string status() const;
    // Drop what is in flight and the ghost (the file closed or switched).
    void reset();

    // Part of a manual beside the exe (docs/<name> in a source tree): from the
    // line starting with `from` to the one starting with `to` ("" = the
    // start / the end). "" when the file is not there.
    static std::string manual(const std::string& name, const std::string& from = std::string(),
                              const std::string& to = std::string());

private:
    friend struct CodeAssistProbe;   // tools/assistcheck.cpp looks inside

    struct Edit {
        bool        open = false, focus = false;
        char        buf[1024] = "";
        CodeEditor::Coordinates start, end;   // the region (empty: insert at start)
        std::string original;                 // the region's text when asked
        std::vector<nlohmann::json> history;  // the conversation so far (refining)
        ollama::ReplyPtr reply;
        std::string result;                   // the code the model wrote
        std::string error;
        bool        haveResult = false;
    };

    void suggest(const Doc& d);
    void takeSuggestion(const Doc& d);
    void startEdit(const Doc& d, const std::string& instruction);
    void sendEdit(const Doc& d, const std::string& instruction);
    void drawEdit(const Doc& d, bool& changed);
    void explain(const Doc& d);
    void fix(const Doc& d);
    void clearGhost();

    CodeEditor* m_ed = nullptr;   // the editor the state below is about

    // Inline suggestion.
    ollama::ReplyPtr m_req;
    std::string      m_reqBefore, m_reqAfter;   // the text around the caret when asked
    CodeEditor::Coordinates m_reqAt;
    std::string      m_reqRest;                 // the caret line's text after the caret
    std::string      m_ghost;                   // what is shown (CodeEditor holds a copy)
    CodeEditor::Coordinates m_ghostAt;
    bool             m_takePending = false, m_takeWord = false;
    bool             m_wantSuggest = false;     // typing happened: ask when it pauses
    double           m_lastType = 0.0;
    double           m_backoffUntil = 0.0;      // the server failed: quiet until then
    std::string      m_lastError;
    bool             m_manual = false;          // asked for (Alt+\, the menu), not by a pause

    Edit m_edit;
};
