#pragma once

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fitzel/graphics/Mesh.hpp>

// Several figures wearing the same animated model, each in its own pose.
//
// An imported model's meshes are shared by every object that shows it, and the
// skinning pass writes a pose into them -- so with two figures of one model the
// last one skinned set the pose of both (a town's people walking in step with
// the player, the player's legs doing what a passer-by's do). The first figure
// of a model each frame still skins into the model's own meshes (one figure
// costs nothing more than before); every further one gets meshes of its own,
// made on its first frame and kept while it keeps being skinned.
class SkinCopies {
public:
    void beginFrame() {
        ++m_frame;
        m_first.clear();
        m_now.clear();
    }

    // Where object `id` showing `model` skins to this frame: null for the
    // model's own meshes (the first figure of it), else its own copy -- empty
    // the first time, filled primitive by primitive (see put).
    std::vector<fitzel::Mesh>* target(int id, const void* model) {
        if (m_first.insert(model).second) return nullptr;
        m_now.insert(id);
        Copy& c = m_copies[id];
        c.lastFrame = m_frame;
        return &c.meshes;
    }

    // Primitive `prim` of a copy, skinned to `verts`.
    static void put(std::vector<fitzel::Mesh>& meshes, std::size_t prim,
                    const std::vector<fitzel::Vertex>& verts) {
        if (prim < meshes.size()) meshes[prim].update(verts);
        else if (prim == meshes.size()) meshes.push_back(fitzel::Mesh::create(verts));
    }

    // Copies not skinned for a few seconds go (a figure removed, or gone
    // inside a house for a while). Kept that long because who counts as the
    // first figure of a model changes when that one is hidden for a moment.
    void endFrame() {
        for (auto it = m_copies.begin(); it != m_copies.end();) {
            if (m_frame - it->second.lastFrame > 600) it = m_copies.erase(it);
            else ++it;
        }
    }

    // What object `id` draws for primitive `prim` this frame: its own copy, or
    // null for the model's meshes.
    const fitzel::Mesh* meshOf(int id, std::size_t prim) const {
        if (!m_now.count(id)) return nullptr;
        const auto it = m_copies.find(id);
        if (it == m_copies.end() || prim >= it->second.meshes.size()) return nullptr;
        return &it->second.meshes[prim];
    }

    void clear() { m_copies.clear(); m_first.clear(); m_now.clear(); }

private:
    struct Copy {
        std::vector<fitzel::Mesh> meshes;
        long long                 lastFrame = 0;
    };
    std::unordered_map<int, Copy>   m_copies;
    std::unordered_set<const void*> m_first;
    std::unordered_set<int>         m_now;
    long long                       m_frame = 0;
};
