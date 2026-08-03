#include "GameSettingsPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "FolderDialog.hpp"
#include "UiStyle.hpp"

namespace game {

namespace {

bool listed(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

bool drawSettingsModal(const char* popupId, Settings& s,
                       const std::vector<std::string>& scenes,
                       const std::string& projectFolder) {
    namespace fs = std::filesystem;
    bool applied = false;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(720.0f, 900.0f));
    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return false;

    // The single free-text field (exe name) is mirrored into a char buffer, synced
    // from `s` on the frame the modal opens so re-opening always shows saved state.
    static char exeBuf[64];
    if (ImGui::IsWindowAppearing())
        std::snprintf(exeBuf, sizeof exeBuf, "%s", s.exeName.c_str());

    // --- Executable name -----------------------------------------------------
    ui::sectionText("Executable");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##exeName", "game (project name)", exeBuf, sizeof exeBuf);
    s.exeName = exeBuf;
    ui::hint("Base name of the exported .exe. Blank uses the project name.");
    ImGui::Spacing();

    // --- Splash image --------------------------------------------------------
    ui::sectionText("Splash screen");
    ImGui::TextUnformatted(s.splash.empty() ? "(engine default)" : s.splash.c_str());
    if (ImGui::Button("Browse...")) {
        std::string picked;
        if (ed::pickFile(picked, projectFolder, "Images",
                         "*.png;*.jpg;*.jpeg;*.bmp;*.tga")) {
            // Copy the chosen image into the project so the export bundles it; store
            // just the file name (project-relative) in the settings.
            std::error_code ec;
            const std::string base = fs::path(picked).filename().string();
            const fs::path dst = fs::path(projectFolder) / base;
            if (fs::weakly_canonical(picked, ec) != fs::weakly_canonical(dst, ec))
                fs::copy_file(picked, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) s.splash = base;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Use default")) s.splash.clear();
    ui::hint("Shown while the game loads. PNG/JPG; blank uses the engine splash.");
    ImGui::Spacing();

    // --- Start scene ---------------------------------------------------------
    ui::sectionText("Start scene");
    const char* startLabel =
        s.startScene.empty() ? "(default scene)" : s.startScene.c_str();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##startScene", startLabel)) {
        if (ImGui::Selectable("(default scene)", s.startScene.empty()))
            s.startScene.clear();
        for (const std::string& stem : scenes)
            if (ImGui::Selectable(stem.c_str(), s.startScene == stem))
                s.startScene = stem;
        ImGui::EndCombo();
    }
    ui::hint("Scene the game boots into. Default is the project's main scene.");
    ImGui::Spacing();

    // --- Export scenes -------------------------------------------------------
    ui::sectionText("Scenes to export");
    bool all = s.exportScenes.empty();
    if (ImGui::Checkbox("All scenes", &all)) {
        if (all) s.exportScenes.clear();
        else     s.exportScenes = scenes; // switch to an explicit list, all selected
    }
    if (!all) {
        ImGui::Indent();
        for (const std::string& stem : scenes) {
            bool inc = listed(s.exportScenes, stem);
            if (ImGui::Checkbox(stem.c_str(), &inc)) {
                if (inc) s.exportScenes.push_back(stem);
                else s.exportScenes.erase(
                         std::remove(s.exportScenes.begin(), s.exportScenes.end(), stem),
                         s.exportScenes.end());
            }
        }
        ImGui::Unindent();
        ui::hint("The start scene is always bundled, even if unchecked.");
    }
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Save", ImVec2(120.0f, 0.0f))) {
        applied = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return applied;
}

} // namespace game
