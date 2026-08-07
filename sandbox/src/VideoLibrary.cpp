#include "VideoLibrary.hpp"

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/asset/AssetTypes.hpp>
#include <fitzel/graphics/VideoTexture.hpp>

std::shared_ptr<fitzel::VideoTexture> VideoLibrary::get(fitzel::AssetDatabase& db,
                                                        fitzel::AssetId id) {
    if (!id.valid()) return nullptr;
    if (auto it = m_byId.find(id); it != m_byId.end()) return it->second;

    std::shared_ptr<fitzel::VideoTexture> v;
    if (db.typeForId(id) == fitzel::AssetType::Video) {
        const std::filesystem::path p = db.pathForId(id);
        if (!p.empty()) v = fitzel::VideoTexture::open(p.string());
    }
    // Stored even when null: that is the "we tried, it didn't work" marker, and
    // without it a broken reference would re-open the file every frame.
    m_byId.emplace(id, v);
    return v;
}

void VideoLibrary::advanceAll(float dt) {
    for (auto& [id, v] : m_byId) {
        (void)id;
        if (v) v->advance(dt);
    }
}

int VideoLibrary::openCount() const {
    int n = 0;
    for (const auto& [id, v] : m_byId) {
        (void)id;
        if (v) ++n;
    }
    return n;
}
