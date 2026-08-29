#include "ModelsPanel.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

namespace modelsui {

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Models", &s.show)) {
ImGui::TextDisabled("Import glTF/GLB from the models/ folder.");
std::error_code mec;
std::vector<std::string> files;
for (const auto& e :
     std::filesystem::directory_iterator(s.modelDir, mec)) {
    if (!e.is_regular_file()) continue;
    std::string ext = e.path().extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    if (ext == ".glb" || ext == ".gltf" || ext == ".dae" || ext == ".fbx")
        files.push_back(e.path().filename().string());
}
std::sort(files.begin(), files.end());
for (const std::string& f : files)
    if (ImGui::Selectable(f.c_str(), s.modelFile == f)) s.modelFile = f;

ImGui::Separator();
ImGui::BeginDisabled(s.modelFile.empty());
if (ImGui::Button("Import to scene")) {
    const std::string path = s.modelDir + "/" + s.modelFile;
    const glm::vec3 p = s.camera.position() + s.camera.front() * 8.0f;
    const glm::vec3 g(p.x, s.streamer.heightAt(p.x, p.z), p.z);
    if (s.isStructured(path)) s.addHierarchy(g, path);
    else {
        const int id = s.models.import(path, s.assetDb, s.materials);
        if (id >= 0) s.addEntity(g, id);
    }
}
ImGui::EndDisabled();
ImGui::TextDisabled("%d model(s) loaded.",
                    static_cast<int>(s.models.size()));
    }
    ImGui::End();
}

} // namespace modelsui
