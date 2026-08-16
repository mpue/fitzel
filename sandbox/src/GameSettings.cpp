#include "GameSettings.hpp"

#include <fstream>

#include <fitzel/asset/Vfs.hpp>
#include <nlohmann/json.hpp>

namespace game {

static std::string settingsPath(const std::string& projectFolder) {
    return projectFolder + "/game.json";
}

Settings load(const std::string& projectFolder) {
    Settings s;
    const std::string body = fitzel::vfs::readText(settingsPath(projectFolder));
    if (body.empty()) return s;
    nlohmann::json j;
    try { j = nlohmann::json::parse(body); }
    catch (const nlohmann::json::exception&) { return s; }
    s.exeName      = j.value("exeName", std::string{});
    s.splash       = j.value("splash", std::string{});
    s.startScene   = j.value("startScene", std::string{});
    s.exportScenes = j.value("exportScenes", std::vector<std::string>{});
    s.trimAssets   = j.value("trimAssets", false);
    s.packContent  = j.value("packContent", true);
    return s;
}

void save(const std::string& projectFolder, const Settings& s) {
    nlohmann::json j;
    j["exeName"]      = s.exeName;
    j["splash"]       = s.splash;
    j["startScene"]   = s.startScene;
    j["exportScenes"] = s.exportScenes;
    j["trimAssets"]   = s.trimAssets;
    j["packContent"]  = s.packContent;
    std::ofstream f(settingsPath(projectFolder));
    if (f) f << j.dump(2) << '\n';
}

} // namespace game
