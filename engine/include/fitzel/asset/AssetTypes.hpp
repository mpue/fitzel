#pragma once

#include <string>
#include <string_view>

namespace fitzel {

// The kind of asset a file represents, decided by its extension during a scan.
// Unknown files are skipped by the database (no .meta, no id).
enum class AssetType {
    Unknown,
    Texture, // png, jpg, jpeg, tga, bmp, exr, hdr
    Model,   // gltf, glb
    Sound,   // wav, ogg, mp3, flac
    Material // .fmat -- native material asset (introduced in Phase 2)
};

// Map a lowercase-or-mixed file extension (with or without leading '.') to a
// type. Returns Unknown for anything the importers don't handle.
AssetType assetTypeForExtension(std::string_view ext);

inline const char* assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Texture:  return "Texture";
        case AssetType::Model:    return "Model";
        case AssetType::Sound:    return "Sound";
        case AssetType::Material: return "Material";
        case AssetType::Unknown:  return "Unknown";
    }
    return "Unknown";
}

// Per-texture import options, persisted in the `.meta` sidecar so re-imports are
// reproducible. `flipVertically` mirrors the argument to Texture::fromFile
// (normal maps shipped bottom-up want it off); `sRGB` is reserved for a future
// colour-space-correct upload path and is not consumed yet.
struct TextureImportSettings {
    bool flipVertically = true;
    bool sRGB           = true;

    bool operator==(const TextureImportSettings& o) const {
        return flipVertically == o.flipVertically && sRGB == o.sRGB;
    }
};

} // namespace fitzel
