#include "ScriptEditor.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <imgui.h>

#include <fitzel/ui/Gui.hpp>

#include "ProjectIO.hpp"
#include "ScriptSystem.hpp"

ScriptEditor::ScriptEditor() {
    m_editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    m_editor.SetPalette(TextEditor::GetDarkPalette());
}

std::vector<std::string> ScriptEditor::list() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& de :
         std::filesystem::directory_iterator(scriptsDir(), ec))
        if (de.is_regular_file() && de.path().extension() == ".lua")
            out.push_back(de.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

void ScriptEditor::open(const std::string& file) {
    if (file.empty()) return;
    const std::string p = path(file);
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
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
            ImGui::EndMenuBar();
        }

        ImGui::Text("%s%s", m_path.empty() ? "(no file)"
                                               : m_path.c_str(),
                    m_dirty ? " *" : "");
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

        if (mono) ImGui::PushFont(mono);
        if (suppressKb) m_editor.SetHandleKeyboardInputs(false);
        m_editor.Render("LuaText");
        if (suppressKb) m_editor.SetHandleKeyboardInputs(true);
        const ImVec2 edMin = ImGui::GetItemRectMin();
        const ImVec2 edMax = ImGui::GetItemRectMax();
        const float  charW = mono ? ImGui::CalcTextSize("A").x : 8.0f;
        const float  lineH = ImGui::GetTextLineHeightWithSpacing();
        if (mono) ImGui::PopFont();

        if (m_editor.IsTextChanged()) m_dirty = true;

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
            ImVec2 at(edMin.x + charW * (6.0f + cur.mColumn),
                      edMin.y + lineH * (cur.mLine + 1));
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
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("Name", m_newName, sizeof(m_newName));
        const char* templates[] = { "Empty component",
                                    "Component (documented)" };
        ImGui::SetNextItemWidth(260.0f);
        ImGui::Combo("Template", &m_newTemplate, templates, 2);
        const std::string safe = projectio::safeName(m_newName);
        const std::string file = safe + ".lua";
        std::error_code sec;
        const bool exists = m_newName[0] &&
            std::filesystem::exists(path(file), sec);
        if (exists)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                               "scripts/%s already exists.", file.c_str());
        ImGui::BeginDisabled(m_newName[0] == '\0' || exists);
        if (ImGui::Button("Create", ImVec2(110.0f, 0.0f))) {
            std::error_code ec;
            std::filesystem::create_directories(scriptsDir(), ec);
            std::ofstream out(path(file));
            if (out) {
                char body[2048];
                std::snprintf(body, sizeof(body),
                    m_newTemplate == 1 ? luacomplete::kTemplateDocumented
                                           : luacomplete::kTemplateEmpty,
                    file.c_str());
                out << body;
            }
            m_newName[0] = '\0';
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
