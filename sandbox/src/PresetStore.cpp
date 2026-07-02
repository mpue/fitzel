#include "PresetStore.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

namespace {
namespace fs = std::filesystem;
const fs::path kPresetDir = "presets";
const char*    kExt       = ".fzp";
} // namespace

void PresetStore::addFloat(const char* key, float& ref) {
    m_fields.push_back({key, [&ref] { return ref; },
                             [&ref](float v) { ref = v; }});
}

void PresetStore::addBool(const char* key, bool& ref) {
    m_fields.push_back({key, [&ref] { return ref ? 1.0f : 0.0f; },
                             [&ref](float v) { ref = v > 0.5f; }});
}

void PresetStore::addInt(const char* key, int& ref) {
    m_fields.push_back({key, [&ref] { return static_cast<float>(ref); },
                             [&ref](float v) { ref = static_cast<int>(std::lround(v)); }});
}

std::vector<std::string> PresetStore::list() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (fs::exists(kPresetDir, ec))
        for (const auto& e : fs::directory_iterator(kPresetDir, ec))
            if (e.path().extension() == kExt) out.push_back(e.path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
}

void PresetStore::save(const std::string& name) const {
    std::error_code ec; fs::create_directories(kPresetDir, ec);
    std::ofstream f(kPresetDir / (name + kExt));
    for (const Field& fld : m_fields) f << fld.key << ' ' << fld.get() << '\n';
}

bool PresetStore::load(const std::string& name) {
    std::ifstream f(kPresetDir / (name + kExt));
    if (!f) return false;
    std::map<std::string, float> vals;
    std::string k; float v;
    while (f >> k >> v) vals[k] = v;
    for (const Field& fld : m_fields) {
        auto it = vals.find(fld.key);
        if (it != vals.end()) fld.set(it->second);
    }
    return true;
}

void PresetStore::remove(const std::string& name) const {
    std::error_code ec; fs::remove(kPresetDir / (name + kExt), ec);
}
