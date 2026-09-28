#include "UiOverlayPanel.hpp"

#include <filesystem>
#include <memory>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "Command.hpp"
#include "ProjectIO.hpp"
#include "UiOverlayCommand.hpp"

namespace uioverlayui {

std::string copyToScene(UiOverlay& overlay, const std::string& currentProject,
                        const std::string& stem) {
    if (currentProject.empty()) return "No project open.";
    const std::filesystem::path cur(currentProject);
    if (cur.stem().string() == stem) return "That's the scene you're editing.";
    const std::filesystem::path target = cur.parent_path() / (stem + ".fitzel");
    std::error_code cec;
    if (!std::filesystem::exists(target, cec)) return "Scene not found: " + stem;
    nlohmann::json keys = nlohmann::json::object();
    overlay.save(keys); // "uiOverlay" + "uiOverlayMenu"
    if (keys.empty()) return "Nothing to copy.";
    if (!projectio::mergeSceneSettings(target.generic_string(), keys))
        return "Could not write " + stem + ".fitzel";
    return "Copied " + std::to_string(overlay.elements().size()) + " element(s) into " + stem +
           " (its previous overlay was replaced).";
}

void panel(UiOverlay& overlay, Bracket& bracket, const Host& h) {
    if (!h.show) return;
    const std::vector<UiElement> frameStart = overlay.elements();

    // Scene names for the LoadScene action picker (stems of the sibling .fitzel
    // files).
    std::vector<std::string> sceneNames;
    if (!h.currentProject.empty()) {
        const std::string folder =
            std::filesystem::path(h.currentProject).parent_path().generic_string();
        for (const auto& sc : projectio::listScenesIn(folder)) sceneNames.push_back(sc.first);
    }

    overlay.drawEditorPanel(&h.show, h.sel, h.assetDb, sceneNames, h.sounds,
                            [&](const std::string& stem) {
                                return copyToScene(overlay, h.currentProject, stem);
                            });

    const bool active  = ImGui::IsAnyItemActive();
    const bool changed = overlay.elements() != frameStart;
    if (changed && !bracket.open) { bracket.open = true; bracket.before = frameStart; }
    if (bracket.open && !active) {
        bracket.open = false;
        auto cmd = std::make_unique<UiOverlayCmd>(overlay, bracket.before, overlay.elements());
        if (!cmd->trivial()) h.history.push(std::move(cmd), h.document);
    }
}

} // namespace uioverlayui
