#pragma once

#include <string>
#include <vector>

#include "GameSettings.hpp"

// Editor-only ImGui dialog for the per-project game::Settings. Kept out of
// GameSettings.cpp (the runtime load/save) so the player -- which never draws UI
// and doesn't link the file dialog -- stays free of ImGui/FolderDialog.
namespace game {

// Draw the "Game Settings" modal. The caller opens it with
// ImGui::OpenPopup("Game Settings") and then calls this every frame while it may
// be open. Edits `s` in place; `scenes` are the project's scene stems (for the
// start-scene picker and the export-scene checklist); `projectFolder` is where a
// picked splash image is copied and the browse dialog starts. Returns true on the
// frame the user presses Save (the caller then persists `s` and re-exports).
bool drawSettingsModal(const char* popupId, Settings& s,
                       const std::vector<std::string>& scenes,
                       const std::string& projectFolder);

} // namespace game
