#include "AssistantPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>

#include <imgui.h>

#include <fitzel/ui/Gui.hpp>

using nlohmann::json;

namespace {

// A turn that calls tools this often without an answer is going in circles.
constexpr int kMaxRounds = 24;
// What one tool's answer may weigh in the conversation; a scene list of a
// thousand objects would leave no room for anything else.
constexpr std::size_t kMaxToolResult = 8000;

const ImVec4 kDim(0.6f, 0.6f, 0.62f, 1.0f);
const ImVec4 kErr(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kBlue(0.55f, 0.75f, 1.0f, 1.0f);

// Markdown's inline marks read as noise in plain ImGui text: the words stay.
std::string plain(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') continue;
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') { ++i; continue; }
        out += s[i];
    }
    return out;
}

std::string shortJson(const json& j, std::size_t max) {
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    if (s.size() > max) s = s.substr(0, max) + "...";
    return s;
}

// What a tool did, in a few words for its row.
std::string outcome(const std::string& content, bool failed) {
    const json j = json::parse(content, nullptr, false);
    if (failed && j.is_object() && j.contains("error") && j["error"].is_string()) return j["error"].get<std::string>();
    if (!j.is_object()) return content.substr(0, 80);
    if (j.contains("created") && j["created"].is_object())
        return "made \"" + j["created"].value("name", std::string()) + "\" (id " +
               std::to_string(j["created"].value("id", 0)) + ")";
    if (j.contains("deleted")) return "deleted " + shortJson(j["deleted"], 60);
    if (j.contains("copy") && j["copy"].is_object())
        return "copied as \"" + j["copy"].value("name", std::string()) + "\" (id " +
               std::to_string(j["copy"].value("id", 0)) + ")";
    if (j.contains("object") && j["object"].is_object())
        return "\"" + j["object"].value("name", std::string()) + "\" (id " + std::to_string(j["object"].value("id", 0)) +
               ")";
    if (j.contains("properties")) return "changed";
    if (j.contains("objects") && j["objects"].is_array() && !j.contains("count"))
        return std::to_string(j["objects"].size()) + " objects";
    if (j.contains("selected")) return j["selected"].is_array() ? std::to_string(j["selected"].size()) + " selected" : "selected";
    if (j.contains("undone")) return "undid " + std::to_string(j["undone"].get<int>());
    if (j.contains("redone")) return "redid " + std::to_string(j["redone"].get<int>());
    if (j.contains("component_types") && j["component_types"].is_array())
        return std::to_string(j["component_types"].size()) + " component types";
    if (j.contains("content") && j.contains("file")) return "read " + j.value("file", std::string());
    if (j.contains("count")) return std::to_string(j["count"].get<int>()) + " objects";
    if (j.contains("scripts") && j["scripts"].is_array()) return std::to_string(j["scripts"].size()) + " scripts";
    if (j.contains("file") && j.contains("replaced"))
        return std::string(j.value("replaced", false) ? "rewrote " : "wrote ") + j.value("file", std::string());
    if (j.contains("lines") && j["lines"].is_array()) return std::to_string(j["lines"].size()) + " log lines";
    if (j.contains("reference")) return "read the reference";
    if (j.contains("ok")) return "done";
    if (j.contains("id") && j.contains("name"))
        return "\"" + j.value("name", std::string()) + "\" (id " + std::to_string(j.value("id", 0)) + ")";
    return shortJson(j, 80);
}

} // namespace

AssistantPanel::AssistantPanel() {
    ollama::client().load();
    std::snprintf(m_url, sizeof m_url, "%s", ollama::client().settings.url.c_str());
}

void AssistantPanel::ask(const std::string& text) {
    visible = true;
    if (busy()) {
        // A turn is under way: the question waits in the box.
        std::snprintf(m_input, sizeof m_input, "%s", text.c_str());
        m_focusInput = true;
        return;
    }
    send(text);
}

std::string AssistantPanel::instructions() const {
    std::string s =
        "You are the assistant built into Fitzel, a 3D game engine and its editor. You help the user "
        "build their game: you look at and change the open scene with your tools, write scripts and "
        "attach them to objects, try the game with play/stop, and explain.\n"
        "- Units are metres, Y is up, the ground is near y = 0. Rotations are degrees. half_size is HALF "
        "an object's extent (a 2 m wide box has half_size 1).\n"
        "- Name objects by id when you know it (tool answers give ids), otherwise by name.\n"
        "- Do things with the tools instead of telling the user how to. Look before you change "
        "(list_objects, get_object); several small tool calls are fine.\n"
        "- A built-in component does many jobs without a script (spin turns an object, physics makes "
        "it fall and collide, ...): prefer one when it fits.\n"
        "- Scripts use ONLY the engine API below (and the language's standard library) -- no "
        "functions or fields from other engines. For details look up scripting_reference. After "
        "attaching a script you may play, read_log, and stop.\n"
        "- Every scene change can be undone with Ctrl+Z (or the undo tool).\n"
        "- When done, say briefly what you did. Answer in the language the user writes in.";
    if (brief) s += "\n\nRight now: " + brief();
    if (reference) {
        const std::string r = reference();
        if (!r.empty()) s += "\n\nThe scripting API:\n" + r;
    }
    return s;
}

json AssistantPanel::requestMessages() const {
    // The newest messages that fit: the context holds roughly 3 characters a
    // token, and the instructions and the tools take their share first.
    const std::size_t budget = static_cast<std::size_t>(
        std::max(6000, std::max(4096, ollama::client().settings.contextSize) * 3 - 14000));
    std::size_t used = 0;
    std::size_t first = m_msgs.size();
    while (first > 0) {
        const Msg& m = m_msgs[first - 1];
        const std::size_t w = m.content.size() + (m.toolCalls.is_array() ? m.toolCalls.dump().size() : 0) + 32;
        if (used + w > budget && used > 0) break;
        used += w;
        --first;
    }
    // Start at a question: a tool answer without its call confuses a model.
    while (first < m_msgs.size() && m_msgs[first].role != "user") ++first;
    json out = json::array();
    out.push_back({{"role", "system"}, {"content", instructions()}});
    for (std::size_t i = first; i < m_msgs.size(); ++i) {
        const Msg& m = m_msgs[i];
        json j = {{"role", m.role}, {"content", m.content}};
        if (m.role == "assistant" && m.toolCalls.is_array() && !m.toolCalls.empty()) j["tool_calls"] = m.toolCalls;
        if (m.role == "tool") j["tool_name"] = m.toolName;
        out.push_back(std::move(j));
    }
    return out;
}

void AssistantPanel::send(const std::string& text) {
    if (text.empty()) return;
    Msg u;
    u.role    = "user";
    u.content = text;
    m_msgs.push_back(std::move(u));
    m_rounds   = 0;
    m_allowAll = false;
    round();
}

void AssistantPanel::round() {
    ollama::Client& oc = ollama::client();
    json toolDefs = json::array();
    for (const assistant::Tool& t : tools)
        toolDefs.push_back({{"type", "function"},
                            {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
    json body = {{"messages", requestMessages()}};
    if (!toolDefs.empty()) body["tools"] = toolDefs;
    Msg a;
    a.role = "assistant";
    m_msgs.push_back(std::move(a));
    m_reply      = oc.chat(body);
    m_scrollDown = true;
    ++m_rounds;
}

void AssistantPanel::stop() {
    if (m_reply) ollama::client().cancel(m_reply);
    for (const Call& c : m_calls) {
        Msg t;
        t.role     = "tool";
        t.toolName = c.name;
        t.args     = c.args;
        t.content  = json{{"error", "stopped by the user"}}.dump();
        t.denied   = true;
        m_msgs.push_back(std::move(t));
    }
    m_calls.clear();
    m_waitApproval = false;
}

void AssistantPanel::runCall(const Call& c) {
    Msg t;
    t.role     = "tool";
    t.toolName = c.name;
    t.args     = c.args;
    const auto it = std::find_if(tools.begin(), tools.end(), [&](const assistant::Tool& x) { return x.name == c.name; });
    json result;
    if (it == tools.end()) {
        result = {{"error", "there is no tool called " + c.name}};
    } else {
        try {
            result = it->run(c.args);
        } catch (const std::exception& e) {
            result = {{"error", std::string("the arguments did not fit: ") + e.what()}};
        }
    }
    t.failed  = result.is_object() && result.contains("error") && !result["error"].is_null();
    t.content = result.dump(-1, ' ', false, json::error_handler_t::replace);
    if (t.content.size() > kMaxToolResult) t.content = t.content.substr(0, kMaxToolResult) + " ...(cut)";
    m_msgs.push_back(std::move(t));
    m_scrollDown = true;
}

void AssistantPanel::step() {
    if (m_reply) {
        Msg& a = m_msgs.back();
        bool done = false;
        json calls;
        {
            std::lock_guard<std::mutex> g(m_reply->lock);
            a.content  = m_reply->content;
            a.thinking = m_reply->thinking;
            if (m_reply->done) {
                done      = true;
                a.seconds = m_reply->seconds;
                a.tokens  = m_reply->tokens;
                if (!m_reply->ok) {
                    a.failed = true;
                    if (a.content.empty()) a.content = m_reply->cancelled ? "(stopped)" : m_reply->error;
                }
                calls = m_reply->toolCalls;
            }
        }
        if (!done) return;
        m_reply.reset();
        a.content = ollama::withoutThinking(a.content);
        // A call the server did not recognise stays in the text as
        // <tool_call>{...}</tool_call> (Qwen's own form): taken out, and run
        // when it is whole.
        for (std::size_t at; (at = a.content.find("<tool_call>")) != std::string::npos;) {
            const std::size_t end = a.content.find("</tool_call>", at);
            const std::string inner = a.content.substr(at + 11, end == std::string::npos ? std::string::npos : end - at - 11);
            const json j = json::parse(inner, nullptr, false);
            if (!a.failed && j.is_object() && j.contains("name") && j["name"].is_string()) {
                if (!calls.is_array()) calls = json::array();
                calls.push_back({{"function", {{"name", j["name"]}, {"arguments", j.value("arguments", json::object())}}}});
            }
            a.content.erase(at, end == std::string::npos ? std::string::npos : end + 12 - at);
        }
        a.content = ollama::withoutThinking(a.content);
        if (!a.failed && calls.is_array() && !calls.empty()) {
            a.toolCalls = calls;
            for (const json& c : calls) {
                if (!c.contains("function") || !c["function"].is_object()) continue;
                Call call;
                call.name = c["function"].value("name", std::string());
                json args = c["function"].contains("arguments") ? c["function"]["arguments"] : json::object();
                // Some models send the arguments as a JSON string.
                if (args.is_string()) args = json::parse(args.get<std::string>(), nullptr, false);
                call.args = args.is_object() ? args : json::object();
                m_calls.push_back(std::move(call));
            }
        }
        m_scrollDown = true;
    }
    // The calls, in order; the ones that change things wait for a yes when asked to.
    const bool ask = ollama::client().settings.confirmChanges && !m_allowAll;
    while (!m_calls.empty()) {
        const Call& c = m_calls.front();
        const auto it = std::find_if(tools.begin(), tools.end(), [&](const assistant::Tool& x) { return x.name == c.name; });
        if (ask && it != tools.end() && it->changes) {
            m_waitApproval = true;
            return;
        }
        runCall(c);
        m_calls.erase(m_calls.begin());
        // A changed scene shows before the next call runs.
        if (it != tools.end() && it->changes) break;
    }
    if (!m_calls.empty()) return;
    m_waitApproval = false;
    // Every call answered: the model goes on, unless it has gone on too long.
    if (!m_msgs.empty() && m_msgs.back().role == "tool") {
        if (m_rounds >= kMaxRounds) {
            Msg n;
            n.role    = "assistant";
            n.content = "(stopped after " + std::to_string(kMaxRounds) + " rounds of tool calls -- say \"go on\" to continue)";
            n.failed  = true;
            m_msgs.push_back(std::move(n));
            return;
        }
        round();
    }
}

// --- Drawing -------------------------------------------------------------------------

void AssistantPanel::drawText(const std::string& text, fitzel::Gui& gui, int id) {
    // Paragraphs wrapped, ``` blocks in the code font with a Copy button.
    std::size_t at = 0;
    int block = 0;
    while (at < text.size()) {
        const std::size_t fence = text.find("```", at);
        const std::string prose = text.substr(at, fence == std::string::npos ? std::string::npos : fence - at);
        std::size_t p = 0;
        while (p < prose.size()) {
            const std::size_t nl = prose.find('\n', p);
            std::string line = prose.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
            p = nl == std::string::npos ? prose.size() : nl + 1;
            if (line.empty()) {
                ImGui::Spacing();
                continue;
            }
            if (line[0] == '#') {
                const std::size_t s = line.find_first_not_of("# ");
                if (gui.boldFont()) ImGui::PushFont(gui.boldFont());
                ImGui::TextWrapped("%s", plain(s == std::string::npos ? line : line.substr(s)).c_str());
                if (gui.boldFont()) ImGui::PopFont();
                continue;
            }
            if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
                ImGui::Bullet();
                ImGui::SameLine();
                line = line.substr(2);
            }
            ImGui::TextWrapped("%s", plain(line).c_str());
        }
        if (fence == std::string::npos) break;
        const std::size_t bodyStart = text.find('\n', fence);
        if (bodyStart == std::string::npos) break;
        const std::size_t close = text.find("```", bodyStart + 1);
        std::string code = text.substr(bodyStart + 1, close == std::string::npos ? std::string::npos : close - bodyStart - 1);
        while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
        ImGui::PushID(id * 64 + block++);
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        const int lines = (int)std::count(code.begin(), code.end(), '\n') + 1;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.11f, 1.0f));
        if (ImGui::BeginChild("##code", ImVec2(-1.0f, std::min(420.0f, lines * lh + 16.0f)), true,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            if (gui.monoFont()) ImGui::PushFont(gui.monoFont());
            ImGui::TextUnformatted(code.c_str());
            if (gui.monoFont()) ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(code.c_str());
        ImGui::PopID();
        if (close == std::string::npos) break;
        at = close + 3;
        const std::size_t eol = text.find('\n', at);
        at = eol == std::string::npos ? text.size() : eol + 1;
    }
}

void AssistantPanel::drawMessages(fitzel::Gui& gui) {
    if (m_msgs.empty()) {
        ImGui::TextDisabled("Ask something, or tell it what to do in the scene:");
        static const char* examples[] = {
            "What is in this scene?",
            "Put a red 2 m cube on the ground in front of me and make it spin slowly.",
            "Write a script that lets me move the selected object with WASD.",
            "Build a small staircase of 8 steps out of boxes.",
        };
        for (const char* e : examples)
            if (ImGui::Selectable(e)) send(e);
        return;
    }
    const float wrap = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < (int)m_msgs.size(); ++i) {
        Msg& m = m_msgs[i];
        ImGui::PushID(i);
        if (m.role == "user") {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.17f, 0.21f, 0.28f, 1.0f));
            const ImVec2 sz = ImGui::CalcTextSize(m.content.c_str(), nullptr, false, wrap - 20.0f);
            if (ImGui::BeginChild("##user", ImVec2(-1.0f, sz.y + 14.0f), false))
            {
                ImGui::SetCursorPos(ImVec2(8.0f, 7.0f));
                ImGui::PushTextWrapPos(wrap - 10.0f);
                ImGui::TextUnformatted(m.content.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        } else if (m.role == "assistant") {
            if (!m.thinking.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                if (ImGui::TreeNode("##think", "Thinking (%d words)",
                                    (int)std::count(m.thinking.begin(), m.thinking.end(), ' ') + 1)) {
                    ImGui::TextWrapped("%s", m.thinking.c_str());
                    ImGui::TreePop();
                }
                ImGui::PopStyleColor();
            }
            if (m.failed) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErr);
                ImGui::TextWrapped("%s", m.content.c_str());
                ImGui::PopStyleColor();
            } else if (!m.content.empty()) {
                drawText(m.content, gui, i);
            } else if (m_reply && i == (int)m_msgs.size() - 1) {
                const int dots = (int)(ImGui::GetTime() * 3.0) % 4;
                ImGui::TextDisabled("%s%s", m.thinking.empty() ? "Thinking" : "Reasoning", std::string(dots, '.').c_str());
            }
            if (m.seconds > 0.0 && (m.toolCalls.is_null() || m.toolCalls.empty()))
                ImGui::TextDisabled("%.1f s, %lld tokens", m.seconds, m.tokens);
        } else if (m.role == "tool") {
            // One row a call: what it did, the details folded in.
            ImGui::PushStyleColor(ImGuiCol_Text, m.failed || m.denied ? kErr : kBlue);
            const std::string label = m.toolName + ": " + (m.denied ? std::string("not run") : outcome(m.content, m.failed));
            const bool open = ImGui::TreeNodeEx("##tool", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label.c_str());
            ImGui::PopStyleColor();
            if (open) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                if (gui.monoFont()) ImGui::PushFont(gui.monoFont());
                ImGui::TextWrapped("in:  %s", shortJson(m.args, 2000).c_str());
                ImGui::TextWrapped("out: %s", m.content.substr(0, 3000).c_str());
                if (gui.monoFont()) ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
    // A call waiting for the user's word.
    if (m_waitApproval && !m_calls.empty()) {
        const Call& c = m_calls.front();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.28f, 0.22f, 0.10f, 1.0f));
        if (ImGui::BeginChild("##approve", ImVec2(-1.0f, 0.0f), ImGuiChildFlags_AutoResizeY)) {
            ImGui::SetCursorPos(ImVec2(8.0f, 6.0f));
            ImGui::TextWrapped("The assistant wants to %s:", c.name.c_str());
            if (gui.monoFont()) ImGui::PushFont(gui.monoFont());
            ImGui::TextWrapped("%s", shortJson(c.args, 1500).c_str());
            if (gui.monoFont()) ImGui::PopFont();
            ImGui::SetCursorPosX(8.0f);
            if (ImGui::Button("Allow")) {
                runCall(c);
                m_calls.erase(m_calls.begin());
                m_waitApproval = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Allow all this time")) {
                m_allowAll     = true;
                m_waitApproval = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Refuse")) {
                Msg t;
                t.role     = "tool";
                t.toolName = c.name;
                t.args     = c.args;
                t.denied   = true;
                t.content  = json{{"error", "the user did not allow this"}}.dump();
                m_msgs.push_back(std::move(t));
                m_calls.erase(m_calls.begin());
                m_waitApproval = false;
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void AssistantPanel::drawModelCombo(const char* label, std::string& model, bool allowSame) {
    ollama::Client& oc = ollama::client();
    const std::string shown = model.empty() ? (allowSame ? "(same as the chat)" : "(first coder found: " + oc.chatModel() + ")") : model;
    if (ImGui::BeginCombo(label, shown.c_str())) {
        if (ImGui::Selectable(allowSame ? "(same as the chat)" : "(automatic)", model.empty())) {
            model.clear();
            oc.save();
        }
        for (const std::string& m : oc.models())
            if (ImGui::Selectable(m.c_str(), m == model)) {
                model = m;
                oc.save();
            }
        ImGui::EndCombo();
    }
}

void AssistantPanel::drawSettings() {
    if (!m_settings) return;
    ollama::Client& oc = ollama::client();
    ollama::Settings& s = oc.settings;
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("AI settings", &m_settings, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextDisabled("A language model on this machine or the network, through Ollama (ollama.com).");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText("Server", m_url, sizeof m_url, ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            s.url = m_url;
            oc.save();
            oc.refreshModels();
        }
        ImGui::SameLine();
        if (ImGui::Button("Connect")) {
            s.url = m_url;
            oc.save();
            oc.refreshModels();
        }
        if (oc.modelsPending()) ImGui::TextDisabled("Asking the server...");
        else if (!oc.modelsError().empty()) ImGui::TextColored(kErr, "%s", oc.modelsError().c_str());
        else ImGui::TextDisabled("%d models installed.", (int)oc.models().size());

        ImGui::SeparatorText("Models");
        ImGui::SetNextItemWidth(300.0f);
        drawModelCombo("Assistant", s.chatModel, false);
        ImGui::SetItemTooltip("Talks and works the editor through tools -- it must support tools\n"
                              "(qwen3-coder, qwen2.5-coder, llama3.1, mistral-small, ...).");
        ImGui::SetNextItemWidth(300.0f);
        drawModelCombo("Code", s.codeModel, true);
        ImGui::SetItemTooltip("Suggestions and Ctrl+I in the code editors. A coder model that fills\n"
                              "in the middle (qwen2.5-coder, qwen3-coder, codellama ...) suggests best.");
        ImGui::SetNextItemWidth(300.0f);
        int ctx = s.contextSize;
        if (ImGui::InputInt("Context (tokens)", &ctx, 4096, 16384, ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            s.contextSize = std::clamp(ctx, 2048, 262144);
            oc.save();
        }
        ImGui::SetItemTooltip("How much the model reads at once. More remembers more of a conversation and\n"
                              "takes more graphics memory. Changing it reloads the model.");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::SliderFloat("Temperature", &s.temperature, 0.0f, 1.2f, "%.2f")) oc.save();

        ImGui::SeparatorText("Code editors");
        if (ImGui::Checkbox("Suggest while typing", &s.inlineAuto)) oc.save();
        ImGui::SetItemTooltip("Greyed text at the caret when typing pauses: Tab takes it, Esc lets it go.\n"
                              "Alt+\\ (Alt+# on German keys) asks at any time.");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::SliderFloat("Pause before asking", &s.inlineDelay, 0.1f, 2.0f, "%.2f s")) oc.save();
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::SliderInt("Lines at most", &s.inlineMaxLines, 1, 40)) oc.save();

        ImGui::SeparatorText("Assistant");
        if (ImGui::Checkbox("Ask before changes", &s.confirmChanges)) oc.save();
        ImGui::SetItemTooltip("Every change to the scene or a file waits for Allow.\n"
                              "Off: it acts, and Ctrl+Z takes scene changes back.");
    }
    ImGui::End();
}

void AssistantPanel::panel(fitzel::Gui& gui) {
    ollama::client().poll();
    if (m_reply || !m_calls.empty()) step();
    drawSettings();
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Assistant", &visible, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }
    ollama::Client& oc = ollama::client();
    if (ImGui::BeginMenuBar()) {
        if (ImGui::MenuItem("New chat", nullptr, false, !busy())) m_msgs.clear();
        if (ImGui::MenuItem("Settings")) m_settings = true;
        const std::string model = oc.chatModel();
        ImGui::TextDisabled("%s", model.empty() ? (oc.modelsError().empty() ? "no model yet" : "no server")
                                                : model.c_str());
        ImGui::EndMenuBar();
    }
    if (oc.chatModel().empty() && !oc.modelsPending()) {
        ImGui::TextColored(kErr, "%s", oc.modelsError().empty() ? "No model." : oc.modelsError().c_str());
        if (ImGui::SmallButton("Try again")) oc.refreshModels();
        ImGui::SameLine();
        if (ImGui::SmallButton("Settings...")) m_settings = true;
    }

    // The conversation, then the box to type in.
    const float inputH = ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    const float footer = inputH + ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    if (ImGui::BeginChild("##chat", ImVec2(0.0f, -footer), false)) {
        drawMessages(gui);
        if (m_scrollDown || (busy() && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)) ImGui::SetScrollHereY(1.0f);
        m_scrollDown = false;
    }
    ImGui::EndChild();

    if (m_focusInput) {
        ImGui::SetKeyboardFocusHere();
        m_focusInput = false;
    }
    const bool sent = ImGui::InputTextMultiline("##ask", m_input, sizeof m_input, ImVec2(-1.0f, inputH),
                                                ImGuiInputTextFlags_EnterReturnsTrue |
                                                    ImGuiInputTextFlags_CtrlEnterForNewLine);
    const bool canSend = !busy() && m_input[0] != '\0';
    if (sent && canSend) {
        const std::string q = m_input;
        m_input[0]   = '\0';
        m_focusInput = true;
        send(q);
    }
    if (busy()) {
        if (ImGui::Button("Stop")) stop();
        ImGui::SameLine();
        ImGui::TextDisabled(m_waitApproval ? "Waiting for your answer" : "Working...");
    } else {
        ImGui::BeginDisabled(!canSend);
        if (ImGui::Button("Send")) {
            const std::string q = m_input;
            m_input[0]   = '\0';
            m_focusInput = true;
            send(q);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Enter sends, Ctrl+Enter is a new line");
    }
    ImGui::End();
}
