#include "VoxelSystem.hpp"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <fitzel/graphics/Shader.hpp>
#include <fitzel/render/Renderer.hpp>

using fitzel::VoxelChunk;
using fitzel::VoxelType;

namespace {

// --- Cheap deterministic value-noise fBm (same shape as the shader's) --------
// Standalone so the voxel world doesn't depend on the terrain generator; enough
// to give the demo some rolling, layered hills to look at.
float hash21(glm::vec2 p, float seed) {
    p = glm::fract((p + seed) * glm::vec2(123.34f, 345.45f));
    p += glm::dot(p, p + 34.345f);
    return glm::fract(p.x * p.y);
}

float vnoise(glm::vec2 p, float seed) {
    glm::vec2 i = glm::floor(p);
    glm::vec2 f = glm::fract(p);
    glm::vec2 u = f * f * (3.0f - 2.0f * f);
    float a = hash21(i, seed);
    float b = hash21(i + glm::vec2(1, 0), seed);
    float c = hash21(i + glm::vec2(0, 1), seed);
    float d = hash21(i + glm::vec2(1, 1), seed);
    return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
}

float fbm(glm::vec2 p, float seed) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 5; ++i) {
        sum += amp * vnoise(p, seed);
        p   *= 2.0f;
        amp *= 0.5f;
    }
    return sum; // ~0..1
}

} // namespace

VoxelSystem::VoxelSystem(fitzel::Shader& lit) : m_mat(lit) {
    m_mat.set("uColorMode", 3); // 3 = per-vertex voxel colour (vPaint.rgb)
    generate();
}

void VoxelSystem::generate() {
    m_chunks.clear();
    constexpr int N = VoxelChunk::kSize;

    for (int cz = 0; cz < chunksZ; ++cz) {
        for (int cx = 0; cx < chunksX; ++cx) {
            VoxelChunk chunk;
            const glm::ivec3 origin(cx * N, 0, cz * N);

            for (int lz = 0; lz < N; ++lz) {
                for (int lx = 0; lx < N; ++lx) {
                    const float wx = static_cast<float>(origin.x + lx);
                    const float wz = static_cast<float>(origin.z + lz);
                    const float n  = fbm(glm::vec2(wx, wz) * frequency, seed);
                    const int surface = baseHeight +
                        static_cast<int>(std::round(n * heightScale));

                    for (int ly = 0; ly < N; ++ly) {
                        VoxelType t = VoxelType::Air;
                        if (ly > surface) {
                            if (ly <= waterLevel) t = VoxelType::Water; // fill hollows
                        } else if (ly == surface) {
                            if (surface >= baseHeight + static_cast<int>(heightScale) - 3)
                                t = VoxelType::Snow;                 // high peaks
                            else if (surface <= waterLevel + 1)
                                t = VoxelType::Sand;                 // shoreline
                            else
                                t = VoxelType::Grass;                // normal ground
                        } else if (ly > surface - 4) {
                            t = VoxelType::Dirt;                     // subsoil
                        } else {
                            t = VoxelType::Stone;                    // bedrock
                        }
                        chunk.set(lx, ly, lz, t);
                    }
                }
            }

            m_chunks.push_back({origin, fitzel::Mesh::create(chunk.buildMeshData())});
        }
    }
}

void VoxelSystem::submit(fitzel::Renderer& renderer) const {
    if (!enabled) return;
    for (const Chunk& c : m_chunks) {
        const glm::mat4 model =
            glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(c.coord) * voxelSize),
                       glm::vec3(voxelSize));
        renderer.submit(c.mesh, m_mat, model, /*castsPointShadow=*/true);
    }
}

void VoxelSystem::drawPanel() {
    if (!show) return;
    if (ImGui::Begin("Voxels", &show)) {
        ImGui::Checkbox("Draw", &enabled);
        ImGui::TextDisabled("%zu chunks (%d^3 cells each)",
                            static_cast<size_t>(chunksX) * chunksZ, VoxelChunk::kSize);

        bool dirty = false;
        dirty |= ImGui::SliderInt("Chunks X", &chunksX, 1, 8);
        dirty |= ImGui::SliderInt("Chunks Z", &chunksZ, 1, 8);
        dirty |= ImGui::SliderFloat("Seed", &seed, 0.0f, 100.0f);
        dirty |= ImGui::SliderFloat("Frequency", &frequency, 0.005f, 0.12f, "%.3f");
        dirty |= ImGui::SliderFloat("Height", &heightScale, 2.0f, 28.0f);
        dirty |= ImGui::SliderInt("Base height", &baseHeight, 0, 20);
        dirty |= ImGui::SliderInt("Water level", &waterLevel, 0, 20);
        dirty |= ImGui::SliderFloat("Voxel size", &voxelSize, 0.25f, 4.0f);

        if (ImGui::Button("Regenerate") || dirty) generate();
    }
    ImGui::End();
}
