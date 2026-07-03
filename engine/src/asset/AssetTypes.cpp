#include "fitzel/asset/AssetTypes.hpp"

#include <algorithm>
#include <cctype>

namespace fitzel {

AssetType assetTypeForExtension(std::string_view ext) {
    // Normalise: drop a leading dot and lowercase.
    std::string e;
    e.reserve(ext.size());
    for (char c : ext) {
        if (c == '.' && e.empty()) continue;
        e.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga" || e == "bmp" ||
        e == "exr" || e == "hdr")
        return AssetType::Texture;
    if (e == "gltf" || e == "glb")
        return AssetType::Model;
    if (e == "wav" || e == "ogg" || e == "mp3" || e == "flac")
        return AssetType::Sound;
    if (e == "fmat")
        return AssetType::Material;
    return AssetType::Unknown;
}

} // namespace fitzel
