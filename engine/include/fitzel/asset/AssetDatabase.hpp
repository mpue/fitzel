#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/asset/AssetTypes.hpp>

namespace fitzel {

class Texture;
struct ModelData;

// Where an asset comes from. Engine assets ship with the engine (the built-in
// textures/models/sounds); Project assets are added by the user for a specific
// project. A scene references either kind uniformly by GUID -- the database
// resolves the id against whichever mounted source contains it.
enum class AssetSourceKind { Engine, Project };

// A Unity-style asset registry that can mount several content roots at once
// (e.g. an engine source plus a project source). Every asset file on disk gets a
// stable GUID stored in a `<file>.meta` sidecar; references elsewhere (scenes,
// materials) use that GUID rather than a path, so assets can be moved or renamed
// -- or live in a different source -- without breaking links. Decoded assets are
// handed out as shared, deduplicated handles.
class AssetDatabase {
public:
    struct Source {
        std::string           name;
        AssetSourceKind       kind = AssetSourceKind::Engine;
        std::filesystem::path root; // absolute, canonicalised
    };

    struct Entry {
        AssetId               id;
        std::filesystem::path absPath;      // absolute path on disk
        std::string           relPath;      // '/'-separated, relative to its source
        AssetType             type        = AssetType::Unknown;
        int                   sourceIndex = -1; // index into sources()
        TextureImportSettings textureImport;    // meaningful when type == Texture
    };

    AssetDatabase() = default;
    // Convenience: construct with a single Engine source mounted at `engineRoot`.
    explicit AssetDatabase(std::filesystem::path engineRoot);

    // Mount a content root. Returns its source index; re-mounting the same root
    // (by canonical path) just returns the existing index. Call refresh() after
    // mounting to scan it.
    int mount(std::string name, AssetSourceKind kind, std::filesystem::path root);

    // Mount `root` as the single Project source, replacing any previously mounted
    // Project source (Engine sources stay). Call refresh() afterwards.
    int mountProject(std::filesystem::path root);

    // Remove all Project sources (Engine sources stay). Their registry entries
    // persist until the next refresh().
    void unmountProjects();

    const std::vector<Source>& sources() const { return m_sources; }

    // (Re)scan every mounted source from disk. Registers each known asset,
    // creating a `.meta` sidecar with a fresh GUID for files that lack one and
    // reading the GUID + import settings from existing sidecars. GUIDs are stable
    // across refreshes (they live in the sidecar). Weak decode caches survive the
    // rescan. Safe to call repeatedly.
    void refresh();

    // --- Identity -----------------------------------------------------------
    // Resolve a path (absolute, or relative to a source root) to its id. If the
    // file exists under a mounted source and is a known asset type but was not
    // scanned yet, it is imported on the spot. Invalid id for anything outside
    // every mounted source.
    AssetId idForPath(const std::filesystem::path& path);

    std::filesystem::path pathForId(AssetId id) const; // absolute, empty if unknown
    AssetType             typeForId(AssetId id) const;
    AssetSourceKind       sourceKindForId(AssetId id) const; // Engine if unknown
    const Entry*          entry(AssetId id) const;
    const std::vector<AssetId>& allAssets() const { return m_order; }

    // --- Typed, cached loads ------------------------------------------------
    // Deduplicated handle to the decoded asset; repeated calls for the same id
    // return the same object while any caller keeps it alive. nullptr on an
    // unknown/mismatched id or a decode failure.
    std::shared_ptr<Texture>   loadTexture(AssetId id);
    std::shared_ptr<ModelData> loadModelData(AssetId id);

    // Path convenience overloads (resolve, then load).
    std::shared_ptr<Texture>   loadTexture(const std::filesystem::path& path);
    std::shared_ptr<ModelData> loadModelData(const std::filesystem::path& path);

private:
    AssetId     importFile(const std::filesystem::path& absPath, int sourceIndex);
    void        loadOrCreateMeta(Entry& e);
    int         sourceIndexForPath(const std::filesystem::path& absPath) const;
    std::string relKey(const std::filesystem::path& absPath, int sourceIndex) const;
    std::string absKey(const std::filesystem::path& absPath) const;

    std::vector<Source>                      m_sources;
    std::unordered_map<AssetId, Entry>       m_byId;
    std::unordered_map<std::string, AssetId> m_byPath; // key: absolute generic path
    std::vector<AssetId>                     m_order;  // stable listing order

    // Weak dedup caches: alive only while some caller holds the shared handle.
    std::unordered_map<AssetId, std::weak_ptr<Texture>>   m_texCache;
    std::unordered_map<AssetId, std::weak_ptr<ModelData>> m_modelCache;
};

} // namespace fitzel
