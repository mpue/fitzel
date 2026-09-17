#include "SaveData.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace savedata {

std::string safeName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += ok ? c : '_';
    }
    return out.empty() ? std::string("default") : out.substr(0, 64);
}

std::filesystem::path folderFor(const std::string& game) {
    std::filesystem::path base;
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) base = appData;
#else
    if (const char* home = std::getenv("HOME")) base = std::filesystem::path(home) / ".local/share";
#endif
    if (base.empty()) base = ".";
    return base / "fitzel" / "saves" / safeName(game);
}

bool write(const std::string& game, const std::string& slot, const nlohmann::json& value) {
    const std::filesystem::path dir = folderFor(game);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (safeName(slot) + ".json");
    const std::filesystem::path tmp  = dir / (safeName(slot) + ".json.tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << value.dump(2);
        if (!out.good()) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        // Some filesystems refuse to rename over an existing file.
        std::filesystem::remove(file, ec);
        ec.clear();
        std::filesystem::rename(tmp, file, ec);
    }
    return !ec;
}

std::optional<nlohmann::json> read(const std::string& game, const std::string& slot) {
    const std::filesystem::path file = folderFor(game) / (safeName(slot) + ".json");
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

} // namespace savedata
