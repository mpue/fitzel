#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "CSharpLanguage.hpp"
#include "CodeAssist.hpp"
#include "CodeEditor.hpp"

namespace fitzel { class Gui; }
namespace csproject { class Builder; }

// The C# editor: the "C# Editor" window, a small Visual Studio Code inside
// fitzel for the project's scripts. Tabs of open files over a CodeEditor each,
// and behind them the language service (CSharpLanguage.hpp -- Roslyn), which
// gives:
//
//   completion as you type (Tab/Enter accepts, Ctrl+Space asks), with each
//   item's documentation beside the list; signature help inside calls; quick
//   info on hover; errors and warnings squiggled as you type and listed under
//   Problems (with the last build's); semantic colours; F12 / Ctrl+click go to
//   definition; Shift+F12 find references; F2 rename (across files);
//   Shift+Alt+F format; Ctrl+F find, Ctrl+G go to line, Ctrl+/ comment.
//   And a language model's help (CodeAssist.hpp): suggestions as you type,
//   Ctrl+I for a change, Fix with AI on a problem.
//
// Saving writes the file; the project's background build (CSharpProject.hpp)
// picks it up as it always did, and Play saves what is unsaved first. Visual
// Studio stays one menu item away. Editor only.
class CSharpEditor {
public:
    // The open project's folder ("" = none); C# when it has the .csproj.
    std::function<std::string()> projectDir;
    // Open a file (or, empty, the solution) in Visual Studio.
    std::function<void(const std::string& file)> openInIde;
    // The project's background build: its errors join the Problems list,
    // Rebuild starts it.
    csproject::Builder* builder = nullptr;
    // The open scene's objects: offered inside Entity.Find("...") and FindAll.
    struct SceneName { std::string name, kind; };
    std::function<std::vector<SceneName>()> sceneNames;
    bool visible = false;
    // The model's help in the open file; main wires its `ask` to the Assistant.
    CodeAssist assist;

    CSharpEditor();
    ~CSharpEditor();

    // Open a .cs file (absolute path) in a tab and show the window; with a
    // line (1-based), the caret goes there.
    void open(const std::string& path, int line = -1, int column = 1);
    // The window, while `visible`; the language service's replies every frame.
    void panel(fitzel::Gui& gui);
    // The window had the keyboard last frame: Ctrl+S there saves the script,
    // so the editor's own Ctrl+S (the scene) stands back.
    bool focused() const { return m_focused; }
    // Write every changed file. Returns how many were written.
    int saveAll();
    // The file in the front tab ("" = none).
    std::string activePath() const {
        return m_active >= 0 && m_active < (int)m_docs.size() ? m_docs[m_active]->path : std::string();
    }

private:
    friend struct CSharpEditorProbe;   // tools/cseditorcheck.cpp looks inside

    struct Diag {
        CodeEditor::TextPos start, end;
        int         sev = 3;   // 1 info, 2 warning, 3 error
        std::string code, msg;
    };
    struct Doc {
        std::string path;      // absolute, as opened
        std::string title;     // file name
        CodeEditor  ed;
        bool        dirty = false;
        bool        crlf  = false;   // the file had \r\n: written back the same way
        int         version = 0;     // every edit
        double      changedAt = 0.0;
        bool        needAnalyze = true;
        std::int64_t analyzeId = 0;
        int         analyzeVersion = -1;
        std::vector<Diag> diags;
        std::filesystem::file_time_type diskTime{};
    };
    struct CompItem {
        std::string label, filter, sort, inlineDesc;
        int kind = 0, prio = 0, index = 0;
    };

    Doc* active() { return m_active >= 0 && m_active < (int)m_docs.size() ? m_docs[m_active].get() : nullptr; }
    Doc* findDoc(const std::string& path);
    int  indexOf(const Doc* d) const;
    bool save(Doc& d);
    void closeDoc(int index);
    void edited(Doc& d);
    nlohmann::json at(Doc& d, const CodeEditor::Coordinates& c);
    nlohmann::json at(Doc& d) { return at(d, d.ed.GetCursorPosition()); }
    void applyEdits(Doc& d, const nlohmann::json& edits);
    void jump(Doc& d, int line, int ch);   // 0-based

    void handleReplies();
    void requestCompletion(Doc& d, const std::string& trigger);
    void refilter(Doc& d);
    void commitCompletion(Doc& d);
    void closeCompletion();
    void requestSignature(Doc& d);
    void requestAnalyze(Doc& d);
    void goToDefinition(Doc& d, const CodeEditor::Coordinates& c);
    void findReferences(Doc& d);
    void startRename(Doc& d);
    void format(Doc& d);
    void updateFind(Doc& d);
    void findNext(Doc& d, bool backwards);

    void drawEditor(Doc& d, fitzel::Gui& gui, float height);
    CodeAssist::Doc assistDoc(Doc& d);
    void drawCompletion(Doc& d);
    void drawSignature(Doc& d);
    void drawHover(Doc& d, const CodeEditor::Coordinates& at);
    void drawBottom(float height);
    void drawStatus();
    void drawPopups();
    void drawEmpty();

    std::vector<std::unique_ptr<Doc>> m_docs;
    int         m_active    = -1;
    int         m_selectTab = -1;   // force this tab selected next frame
    int         m_askClose  = -1;   // a dirty tab asked to close
    bool        m_focused   = false;
    std::string m_project;          // the project the docs belong to
    cslang::Client m_lang;

    struct {
        bool         open = false;
        std::int64_t reqId = 0;
        Doc*         doc = nullptr;
        std::vector<CompItem> items;
        std::vector<int>      shown;   // indices into items, best first
        int          sel = 0, top = 0;
        CodeEditor::TextPos start;
        std::string  prefix;
        bool         suggestion = false;   // builder mode: Enter does not accept
        bool         stringMode = false;   // names inside a string (scene objects), not code
        std::int64_t descId = 0;
        int          descFor = -1;         // the item index the description is of
        std::string  desc;
        double       selAt = 0.0;
        ImVec2       rectMin, rectMax;     // last frame's list (clicks go here, not the text)
        bool         hadRect = false;
    } m_comp;

    struct {
        bool         open = false;
        std::int64_t reqId = 0;
        Doc*         doc = nullptr;
        nlohmann::json data;
        int          shownSig = -1;        // overload picked with Up/Down (-1: the service's)
        bool         dirty = false;
        double       lastReq = 0.0;
    } m_sig;

    struct {
        std::int64_t reqId = 0;
        Doc*         doc = nullptr;
        CodeEditor::Coordinates a, b;      // the word asked about
        bool         has = false;          // a word is under the mouse
        double       restSince = 0.0;
        ImVec2       lastMouse;
        nlohmann::json data;
        bool         answered = false;
    } m_hover;

    std::int64_t m_defId = 0;
    std::string  m_peek;                   // a definition outside the project: what it is
    ImVec2       m_peekAt;
    std::int64_t m_formatId = 0;
    Doc*         m_formatDoc = nullptr;
    int          m_formatVersion = -1;

    struct Ref { std::string file; int line = 0, ch = 0, len = 0; std::string text; bool def = false; };
    std::vector<Ref> m_refs;
    std::string      m_refsName;
    std::int64_t     m_refsId = 0;

    bool         m_renameOpen = false, m_renameFocus = false;
    char         m_renameBuf[128] = "";
    Doc*         m_renameDoc = nullptr;
    CodeEditor::Coordinates m_renameAt;
    std::int64_t m_renameId = 0;
    std::string  m_renameError;

    bool         m_findOpen = false, m_findFocus = false;
    char         m_findBuf[128] = "";
    std::vector<std::pair<CodeEditor::Coordinates, CodeEditor::Coordinates>> m_matches;
    int          m_findVersion = -1;
    Doc*         m_findDoc = nullptr;
    std::string  m_findText;

    bool         m_gotoOpen = false;
    char         m_gotoBuf[16] = "";
    bool         m_newOpen = false;
    char         m_newBuf[64] = "";

    int          m_bottomTab = 0;          // 0 problems, 1 references
    bool         m_bottomOpen = true;
    std::string  m_status;
    double       m_statusAt = -1e9;
    double       m_lastDiskCheck = 0.0;
    ImFont*      m_mono = nullptr;          // the Gui's monospace font (this frame)
};
