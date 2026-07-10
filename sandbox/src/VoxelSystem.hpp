#pragma once

#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/world/Voxel.hpp>

namespace fitzel {
class Shader;
class Renderer;
}

// A small demo voxel world for the experiment branch: a grid of VoxelChunks
// generated from a blocky height field, each meshed once and drawn through the
// standard lit pipeline (so it gets shadows, fog and SSAO for free). Owns its
// GPU meshes + a flat-colour material; the render loop just calls submit(). Kept
// out of main.cpp so the experiment stays a self-contained subsystem.
class VoxelSystem {
public:
    explicit VoxelSystem(fitzel::Shader& lit);

    // (Re)generate every chunk from the current parameters and re-upload meshes.
    void generate();

    // Submit all chunks to the renderer (world-placed via a per-chunk translate).
    void submit(fitzel::Renderer& renderer) const;

    // ImGui controls: toggle, world size, generation params, regenerate.
    void drawPanel();

    bool show    = false; // panel visibility
    bool enabled = true;  // draw the voxels

    // --- Generation parameters (edited in the panel) -------------------------
    int   chunksX     = 4;      // world size in chunks along X / Z
    int   chunksZ     = 4;
    float seed        = 1.0f;
    float frequency   = 0.03f;  // world units -> noise (smaller = broader hills)
    float heightScale = 18.0f;  // vertical relief in voxels
    int   baseHeight  = 6;      // surface height where the noise is zero
    int   waterLevel  = 7;      // cells at/below this that are air fill with water
    float voxelSize   = 1.0f;   // world units per voxel edge

private:
    struct Chunk {
        glm::ivec3   coord; // chunk grid coordinate
        fitzel::Mesh mesh;
    };

    fitzel::Material     m_mat;
    std::vector<Chunk>   m_chunks;
};
