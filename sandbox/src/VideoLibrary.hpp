#pragma once

#include <memory>
#include <unordered_map>

#include <fitzel/asset/AssetId.hpp>

namespace fitzel {
class AssetDatabase;
class VideoTexture;
}

// The scene's open videos, one per .fvid asset.
//
// Deliberately keyed on the asset, not on the material or the entity: a row of
// billboards showing the same clip decodes it once and shares the texture. That
// is what makes the cost scale with the number of *clips* instead of the number
// of screens -- and for advertising, playing in lockstep is what you'd want
// anyway. Per-screen playback positions would mean one decoder each, which is
// the expensive thing here.
class VideoLibrary {
public:
    // Open `id` (or return the already-open one). Caches failures too, so a
    // missing or corrupt file doesn't get retried every frame. Null on failure.
    std::shared_ptr<fitzel::VideoTexture> get(fitzel::AssetDatabase& db,
                                              fitzel::AssetId id);

    // Step every open video. Cheap on frames where no clip crosses a frame
    // boundary, which at a 15 fps import and 60 fps rendering is most of them.
    void advanceAll(float dt);

    // Drop everything (project close/scene load). Materials holding the textures
    // keep them alive until they let go.
    void clear() { m_byId.clear(); }

    int openCount() const;

private:
    std::unordered_map<fitzel::AssetId, std::shared_ptr<fitzel::VideoTexture>> m_byId;
};
