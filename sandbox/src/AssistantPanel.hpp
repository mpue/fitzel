#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "AssistantTools.hpp"
#include "Ollama.hpp"

namespace fitzel { class Gui; }

// The "Assistant" window: a conversation with a language model (Ollama.hpp)
// that works the editor. It answers questions, and it acts -- the tools
// (AssistantTools.hpp) let it look at the scene, make and change objects,
// write scripts and hang them on objects, press Play and read the log. A turn
// goes round as often as the model calls tools: its calls run here, on the
// main thread, between frames, and their answers go back to it until it
// writes an answer of its own.
//
// "Ask before changes" (the settings) stops before every call that changes
// the scene or a file and lets the user allow or refuse it. Without it the
// model just acts, and the undo history is the way back.
//
// The same window holds the settings every AI feature reads: the server, the
// models, the context size, the editors' suggestions. Editor only.
class AssistantPanel {
public:
    bool visible = false;
    std::vector<assistant::Tool> tools;   // main fills them (makeTools)
    std::function<std::string()> brief;   // the scene in a few lines, for the instructions
    std::function<std::string()> reference;   // the scripting API, for the instructions

    AssistantPanel();

    // Open the window and send `text` as the user's (a code editor's Explain).
    void ask(const std::string& text);
    // Open the settings window.
    void openSettings() { m_settings = true; }
    // The window and the settings, and the conversation's next step -- every
    // frame, also while the window is closed: a turn under way goes on.
    void panel(fitzel::Gui& gui);
    // A turn is under way (the model writing, tools waiting).
    bool busy() const { return (bool)m_reply || !m_calls.empty(); }

private:
    struct Msg {
        std::string    role;        // user | assistant | tool
        std::string    content;
        std::string    thinking;
        nlohmann::json toolCalls;   // an assistant turn's calls (as the model sent them)
        std::string    toolName;    // a tool message: which tool, with what
        nlohmann::json args;
        bool           failed = false;
        bool           denied = false;
        double         seconds = 0.0;
        long long      tokens = 0;
    };
    struct Call {
        std::string    name;
        nlohmann::json args;
    };

    void send(const std::string& text);
    void round();
    void step();
    void runCall(const Call& c);
    void stop();
    nlohmann::json requestMessages() const;
    std::string    instructions() const;

    void drawMessages(fitzel::Gui& gui);
    void drawText(const std::string& text, fitzel::Gui& gui, int id);
    void drawSettings();
    void drawModelCombo(const char* label, std::string& model, bool allowSame);

    std::vector<Msg>  m_msgs;
    ollama::ReplyPtr  m_reply;
    std::vector<Call> m_calls;           // this round's calls still to run
    bool              m_waitApproval = false;
    bool              m_allowAll = false; // "Allow all" for the rest of the turn
    int               m_rounds = 0;
    char              m_input[8192] = "";
    bool              m_focusInput = false;
    bool              m_scrollDown = false;
    bool              m_settings = false;
    char              m_url[256] = "";
};
