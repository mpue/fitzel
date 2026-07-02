#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fitzel {

// One material group of a loaded model: a de-indexed triangle list interleaved
// as position(3) + normal(3) + uv(2) per vertex (8 floats), plus its decoded
// base-colour texture (RGBA, top-to-bottom). Node transforms are baked in.
struct ModelPrimitive {
    std::vector<float>         vertices;  // 8 floats per vertex
    std::vector<std::uint8_t>  texPixels; // RGBA, empty if untextured
    int   texWidth    = 0;
    int   texHeight   = 0;
    bool  alphaCutout = false;            // material uses MASK/BLEND (foliage)
    float baseColor[4] = {0.8f, 0.8f, 0.8f, 1.0f}; // PBR base-colour factor (tint)
    std::string materialName;             // glTF material name (may be empty)

    int vertexCount() const { return static_cast<int>(vertices.size() / 8); }
};

// A loaded glTF/GLB model split into material primitives.
struct ModelData {
    std::vector<ModelPrimitive> primitives;
    float minY = 0.0f;
    float maxY = 0.0f;

    bool  empty()  const { return primitives.empty(); }
    float height() const { return maxY - minY; }
};

// Load a glTF / GLB file (with embedded textures) via cgltf + stb_image.
// Returns an empty ModelData on failure.
ModelData loadGltf(const std::string& path);

} // namespace fitzel
