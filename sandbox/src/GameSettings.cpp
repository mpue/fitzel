#include "GameSettings.hpp"

#include <fstream>

#include <fitzel/asset/Vfs.hpp>
#include <nlohmann/json.hpp>

namespace game {

namespace {

nlohmann::json vec3Json(const glm::vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}
glm::vec3 readVec3(const nlohmann::json& a, glm::vec3 def) {
    if (a.is_array() && a.size() == 3)
        return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
    return def;
}

// The loading screen's style. Every field falls back to the value the struct was
// built with, so a game.json written before this existed -- or one edited by
// hand and missing a key -- keeps today's look instead of a black screen.
loadingscreen::Style readLoading(const nlohmann::json& j) {
    loadingscreen::Style s;
    if (!j.is_object()) return s;
    s.background  = j.value("background", s.background);
    s.fit         = static_cast<loadingscreen::Fit>(
                        j.value("fit", static_cast<int>(s.fit)));
    s.back        = readVec3(j.value("back", nlohmann::json{}), s.back);
    s.title       = j.value("title", s.title);
    s.titleScale  = j.value("titleScale", s.titleScale);
    s.textColor   = readVec3(j.value("textColor", nlohmann::json{}), s.textColor);
    s.barAt       = static_cast<loadingscreen::BarAt>(
                        j.value("barAt", static_cast<int>(s.barAt)));
    s.barInset    = j.value("barInset", s.barInset);
    s.barWidth    = j.value("barWidth", s.barWidth);
    s.barHeight   = j.value("barHeight", s.barHeight);
    s.barRound    = j.value("barRound", s.barRound);
    s.barColor    = readVec3(j.value("barColor", nlohmann::json{}), s.barColor);
    s.barBack     = readVec3(j.value("barBack", nlohmann::json{}), s.barBack);
    s.showLabel   = j.value("showLabel", s.showLabel);
    s.showPercent = j.value("showPercent", s.showPercent);
    s.showVersion = j.value("showVersion", s.showVersion);
    return s;
}

nlohmann::json writeLoading(const loadingscreen::Style& s) {
    nlohmann::json j;
    j["background"]  = s.background;
    j["fit"]         = static_cast<int>(s.fit);
    j["back"]        = vec3Json(s.back);
    j["title"]       = s.title;
    j["titleScale"]  = s.titleScale;
    j["textColor"]   = vec3Json(s.textColor);
    j["barAt"]       = static_cast<int>(s.barAt);
    j["barInset"]    = s.barInset;
    j["barWidth"]    = s.barWidth;
    j["barHeight"]   = s.barHeight;
    j["barRound"]    = s.barRound;
    j["barColor"]    = vec3Json(s.barColor);
    j["barBack"]     = vec3Json(s.barBack);
    j["showLabel"]   = s.showLabel;
    j["showPercent"] = s.showPercent;
    j["showVersion"] = s.showVersion;
    return j;
}

} // namespace

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
    s.icon         = j.value("icon", std::string{});
    s.startScene   = j.value("startScene", std::string{});
    s.exportScenes = j.value("exportScenes", std::vector<std::string>{});
    s.trimAssets   = j.value("trimAssets", false);
    s.packContent  = j.value("packContent", true);
    s.makeInstaller = j.value("makeInstaller", false);
    s.productName   = j.value("productName", std::string{});
    s.version       = j.value("version", std::string("1.0.0"));
    s.publisher     = j.value("publisher", std::string{});
    s.loading      = readLoading(j.value("loading", nlohmann::json{}));
    return s;
}

void save(const std::string& projectFolder, const Settings& s) {
    nlohmann::json j;
    j["exeName"]      = s.exeName;
    j["splash"]       = s.splash;
    j["icon"]         = s.icon;
    j["startScene"]   = s.startScene;
    j["exportScenes"] = s.exportScenes;
    j["trimAssets"]   = s.trimAssets;
    j["packContent"]  = s.packContent;
    j["makeInstaller"] = s.makeInstaller;
    j["productName"]   = s.productName;
    j["version"]       = s.version;
    j["publisher"]     = s.publisher;
    j["loading"]      = writeLoading(s.loading);
    std::ofstream f(settingsPath(projectFolder));
    if (f) f << j.dump(2) << '\n';
}

} // namespace game
