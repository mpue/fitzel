#pragma once

// Internal to stalkerimport: what StalkerImport.cpp and StalkerLife.cpp share
// -- the GLB writer, the GUID a model keeps across imports, and the texture
// conversion.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace xray { class FileSystem; }

namespace stalker::detail {

namespace fs = std::filesystem;
using nlohmann::json;

// --- the GLB --------------------------------------------------------------------

class Glb {
public:
    json gltf;
    std::vector<std::uint8_t> bin;

    Glb() {
        gltf["asset"] = {{"version", "2.0"}, {"generator", "fitzel stalkerimport"}};
        gltf["bufferViews"] = json::array();
        gltf["accessors"] = json::array();
    }

    int view(const void* data, std::size_t bytes, int target) {
        while (bin.size() % 4) bin.push_back(0);
        const std::size_t at = bin.size();
        bin.resize(at + bytes);
        std::memcpy(bin.data() + at, data, bytes);
        json v = {{"buffer", 0}, {"byteOffset", at}, {"byteLength", bytes}};
        if (target) v["target"] = target;
        gltf["bufferViews"].push_back(v);
        return static_cast<int>(gltf["bufferViews"].size()) - 1;
    }

    int accessor(int view, int componentType, std::size_t count, const char* type,
                 const json& mn = nullptr, const json& mx = nullptr) {
        json a = {{"bufferView", view}, {"componentType", componentType},
                  {"count", count}, {"type", type}};
        if (!mn.is_null()) { a["min"] = mn; a["max"] = mx; }
        gltf["accessors"].push_back(a);
        return static_cast<int>(gltf["accessors"].size()) - 1;
    }

    bool write(const fs::path& path) {
        gltf["buffers"] = json::array({{{"byteLength", bin.size()}}});
        std::string text = gltf.dump();
        while (text.size() % 4) text.push_back(' ');
        std::vector<std::uint8_t> body = bin;
        while (body.size() % 4) body.push_back(0);
        const std::uint32_t total = static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + body.size());
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        u32(0x46546C67u);   // "glTF"
        u32(2);
        u32(total);
        u32(static_cast<std::uint32_t>(text.size()));
        u32(0x4E4F534Au);   // "JSON"
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        u32(static_cast<std::uint32_t>(body.size()));
        u32(0x004E4942u);   // "BIN\0"
        out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
        return static_cast<bool>(out);
    }

};


// The GUID of the model: kept from an earlier import, so a scene made then
// still finds it.
std::string modelGuid(const fs::path& glb);

// textures/<tex>.dds -- or the level's own folder first, when `level` is
// given -- halved down to `maxSize` and written as a JPG, or as a PNG keeping
// its alpha.
bool convertTexture(const xray::FileSystem& fs, const std::string& level, const std::string& tex,
                    const fs::path& file, bool alpha, int maxSize, std::string& error);

} // namespace stalker::detail
