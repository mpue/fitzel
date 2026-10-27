#include "ScriptEditor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <imgui.h>

#include <fitzel/ui/Gui.hpp>

#include "CSharpProject.hpp"
#include "ProjectIO.hpp"
#include "ScriptSystem.hpp"

ScriptEditor::ScriptEditor() {
    m_editor.SetLanguageDefinition(CodeEditor::LanguageDefinition::Lua());
}

namespace {
// "scripts/door.lua:12: attempt to ..." -> (11, the message), when the error
// is this file's.
std::vector<std::pair<int, std::string>> problemsIn(const std::string& error, const std::string& path) {
    std::vector<std::pair<int, std::string>> out;
    const std::string name = std::filesystem::path(path).filename().string();
    if (error.empty() || name.empty()) return out;
    const std::size_t at = error.find(name + ":");
    if (at == std::string::npos) return out;
    const int line = std::atoi(error.c_str() + at + name.size() + 1);
    if (line > 0) out.push_back({line - 1, error});
    return out;
}
} // namespace

std::vector<std::string> ScriptEditor::list() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& de :
         std::filesystem::directory_iterator(scriptsDir(), ec))
        if (de.is_regular_file() && de.path().extension() == ".lua")
            out.push_back(de.path().filename().string());
    const std::filesystem::path root(scriptsDir());
    std::vector<std::string> cs;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".cs")
            cs.push_back(it->path().lexically_relative(root).generic_string());
    std::sort(out.begin(), out.end());
    std::sort(cs.begin(), cs.end());
    out.insert(out.end(), cs.begin(), cs.end());
    return out;
}

void ScriptEditor::open(const std::string& file) {
    if (file.empty()) return;
    const std::string p = path(file);
    if (openExternal && std::filesystem::path(file).extension() == ".cs") {
        openExternal(p);
        return;
    }
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
    assist.reset();
    m_editor.SetText(ss.str());
    m_path  = p;
    m_dirty = false;
    visible = true;
}

void ScriptEditor::save(ScriptSystem& scripts) {
    if (m_path.empty()) return;
    std::ofstream out(m_path);
    if (out) { out << m_editor.GetText(); scripts.reset(); m_dirty = false; }
}

void ScriptEditor::panel(ScriptSystem& scripts, fitzel::Gui& gui) {
    m_focused = false;
    if (!visible) return;
    bool openNewScript = false;
    // What the model's help knows of the file (CodeAssist.hpp).
    static const std::string luaApi = luacomplete::apiSummary();
    CodeAssist::Doc assistDoc;
    assistDoc.ed        = &m_editor;
    assistDoc.language  = "lua";
    assistDoc.path      = m_path.empty() ? std::string("scripts/new.lua")
                                         : "scripts/" + std::filesystem::path(m_path).filename().string();
    assistDoc.reference = luaApi;
    assistDoc.problems  = problemsIn(scripts.lastError(), m_path);
    assistDoc.mono      = gui.monoFont();
    if (ImGui::Begin("Scripts", &visible,
                     ImGuiWindowFlags_MenuBar)) {
        bool doSave = false;
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New...")) openNewScript = true;
                if (ImGui::BeginMenu("Open")) {
                    const auto files = list();
                    if (files.empty()) ImGui::TextDisabled("(none)");
                    for (const std::string& f : files)
                        if (ImGui::MenuItem(f.c_str())) open(f);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Save", "Ctrl+S", false,
                                    !m_path.empty()))
                    doSave = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("AI")) {
                assistDoc.ed = m_path.empty() ? nullptr : &m_editor;
                assist.menuItems(assistDoc);
                assistDoc.ed = &m_editor;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        ImGui::Text("%s%s", m_path.empty() ? "(no file)"
                                               : m_path.c_str(),
                    m_dirty ? " *" : "");
        const std::string aiStatus = assist.status();
        if (!aiStatus.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", aiStatus.c_str());
        }
        if (!scripts.lastError().empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                               "  %s", scripts.lastError().c_str());
        }

        // Ctrl+S saves while the editor window is focused.
        const bool winFocused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_focused = winFocused;
        if (winFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
            doSave = true;

        // Code completion: intercept navigate/accept/dismiss keys BEFORE
        // the editor consumes them. We disable the editor's keyboard only
        // on the exact frame we act on a key, so typing is unaffected.
        ImFont* mono = gui.monoFont();
        bool acceptComp = false, suppressKb = false;
        if (m_comp.open && winFocused && !m_comp.items.empty()) {
            const int n = static_cast<int>(m_comp.items.size());
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                m_comp.sel = (m_comp.sel + 1) % n; suppressKb = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                m_comp.sel = (m_comp.sel - 1 + n) % n; suppressKb = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                acceptComp = true; suppressKb = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                m_comp.open = false; suppressKb = true;
                m_comp.manualClose = true; m_comp.closedPrefix = m_comp.prefix;
            }
        }

        // The model's keys: Tab takes its suggestion, Ctrl+I asks for a change.
        if (!m_path.empty() && assist.keys(assistDoc, m_comp.open && !m_comp.items.empty()))
            suppressKb = true;

        if (mono) ImGui::PushFont(mono);
        if (suppressKb) m_editor.SetHandleKeyboardInputs(false);
        m_editor.Render("LuaText");
        if (suppressKb) m_editor.SetHandleKeyboardInputs(true);
        const ImVec2 edMin = ImGui::GetItemRectMin();
        const ImVec2 edMax = ImGui::GetItemRectMax();
        const float  lineH = m_editor.GetLineHeight();
        if (mono) ImGui::PopFont();

        if (m_editor.IsTextChanged()) m_dirty = true;
        if (!m_path.empty() && assist.after(assistDoc, m_comp.open && !m_comp.items.empty()))
            m_dirty = true;

        // Accept the highlighted match: insert the identifier's tail
        // after the already-typed prefix.
        if (acceptComp && m_comp.sel >= 0 && m_comp.sel < static_cast<int>(m_comp.items.size())) {
            const std::string full = m_comp.items[m_comp.sel].text;
            if (full.size() > m_comp.prefix.size())
                m_editor.InsertText(full.substr(m_comp.prefix.size()));
            m_comp.open = false; m_dirty = true;
        }

        // Recompute candidates from the new cursor/text (skip on the
        // frame we suppressed the editor, so navigation/dismiss stick).
        if (!winFocused) m_comp.open = false;
        else if (!suppressKb) luacomplete::refreshCompletion(m_editor, m_comp);

        // Completion popup, best-effort anchored under the caret and
        // clamped inside the editor rect.
        if (m_comp.open && !m_comp.items.empty()) {
            const auto cur = m_editor.GetCursorPosition();
            ImVec2 at = m_editor.CoordinatesToScreen(cur);
            at.y += lineH;
            at.x = std::min(at.x, edMax.x - 300.0f);
            at.y = std::min(at.y, edMax.y - lineH);
            at.x = std::max(at.x, edMin.x);
            at.y = std::max(at.y, edMin.y);
            ImGui::SetNextWindowPos(at);
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(240.0f, 0.0f), ImVec2(520.0f, lineH * 10.0f + 12.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
            if (ImGui::Begin("##luacomplete", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoSavedSettings)) {
                for (int i = 0; i < static_cast<int>(m_comp.items.size()); ++i) {
                    const bool sel = (i == m_comp.sel);
                    if (mono) ImGui::PushFont(mono);
                    if (ImGui::Selectable(m_comp.items[i].text, sel)) {
                        const std::string full = m_comp.items[i].text;
                        if (full.size() > m_comp.prefix.size())
                            m_editor.InsertText(full.substr(m_comp.prefix.size()));
                        m_comp.open = false; m_dirty = true;
                    }
                    if (mono) ImGui::PopFont();
                    if (m_comp.items[i].hint && m_comp.items[i].hint[0]) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", m_comp.items[i].hint);
                    }
                    if (sel) ImGui::SetScrollHereY();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (doSave) save(scripts);
    }
    ImGui::End();

    // New-script modal: create scripts/<name>.lua from a template.
    if (openNewScript) ImGui::OpenPopup("New Script");
    if (ImGui::BeginPopupModal("New Script", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_newLanguage < 0) m_newLanguage = (csharpProject && csharpProject()) ? 1 : 0;
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("Name", m_newName, sizeof(m_newName));
        ImGui::RadioButton("Lua", &m_newLanguage, 0);
        ImGui::SameLine();
        ImGui::RadioButton("C#", &m_newLanguage, 1);
        const bool csharp = m_newLanguage == 1;
        const char* templates[] = { "Empty component",
                                    "Component (documented)" };
        if (!csharp) {
            ImGui::SetNextItemWidth(260.0f);
            ImGui::Combo("Template", &m_newTemplate, templates, 2);
        } else if (!(csharpProject && csharpProject())) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                               "The project has no C# set up (File > Game Settings).");
        }
        // A C# class name is the file name: letters, digits, underscores, and
        // not a digit first.
        std::string safe = projectio::safeName(m_newName);
        if (csharp) {
            std::string id;
            for (char ch : safe)
                if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') id += ch;
            if (!id.empty() && std::isdigit(static_cast<unsigned char>(id[0]))) id = "_" + id;
            safe = id;
        }
        const std::string file = safe + (csharp ? ".cs" : ".lua");
        std::error_code sec;
        const bool exists = m_newName[0] &&
            std::filesystem::exists(path(file), sec);
        if (exists)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                               "scripts/%s already exists.", file.c_str());
        ImGui::BeginDisabled(m_newName[0] == '\0' || safe.empty() || exists);
        if (ImGui::Button("Create", ImVec2(110.0f, 0.0f))) {
            std::error_code ec;
            std::filesystem::create_directories(scriptsDir(), ec);
            std::ofstream out(path(file));
            if (out && csharp) {
                out << csproject::scriptTemplate(safe);
            } else if (out) {
                char body[2048];
                std::snprintf(body, sizeof(body),
                    m_newTemplate == 1 ? luacomplete::kTemplateDocumented
                                           : luacomplete::kTemplateEmpty,
                    file.c_str());
                out << body;
            }
            m_newName[0] = '\0';
            m_newLanguage = -1;
            out.close();
            open(file);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
