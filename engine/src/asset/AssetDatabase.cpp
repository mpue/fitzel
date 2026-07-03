#include "fitzel/asset/AssetDatabase.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "fitzel/graphics/Texture.hpp"
#include "fitzel/world/Model.hpp"

namespace fitzel {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Sidecar path for an asset: "foo.png" -> "foo.png.meta".
fs::path metaPathFor(const fs::path& asset) {
    fs::path m = asset;
    m += ".meta";
    return m;
}

// Lowercase extension without the leading dot ("PNG" -> "png").
std::string lowerExt(const fs::path& p) {
    std::string ext = p.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

const char* sourceKindName(AssetSourceKind k) {
    return k == AssetSourceKind::Project ? "Project" : "Engine";
}

// Canonicalise a path for use as a stable map key / prefix comparison. Tolerates
// not-yet-existing paths (weakly_canonical) and always uses forward slashes.
fs::path canonicalPath(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

} // namespace

AssetDatabase::AssetDatabase(fs::path engineRoot) {
    mount("engine", AssetSourceKind::Engine, std::move(engineRoot));
}

int AssetDatabase::mount(std::string name, AssetSourceKind kind, fs::path root) {
    const fs::path canon = canonicalPath(root);
    for (int i = 0; i < static_cast<int>(m_sources.size()); ++i)
        if (m_sources[i].root == canon) return i; // already mounted
    m_sources.push_back(Source{std::move(name), kind, canon});
    return static_cast<int>(m_sources.size()) - 1;
}

std::string AssetDatabase::absKey(const fs::path& absPath) const {
    return canonicalPath(absPath).generic_string();
}

int AssetDatabase::sourceIndexForPath(const fs::path& absPath) const {
    const std::string key = absKey(absPath);
    int best = -1;
    std::size_t bestLen = 0;
    for (int i = 0; i < static_cast<int>(m_sources.size()); ++i) {
        const std::string rootKey = m_sources[i].root.generic_string();
        // Prefix match on a path boundary; keep the longest (most specific) root.
        if (key.size() >= rootKey.size() &&
            key.compare(0, rootKey.size(), rootKey) == 0 &&
            (key.size() == rootKey.size() || key[rootKey.size()] == '/')) {
            if (rootKey.size() > bestLen) { best = i; bestLen = rootKey.size(); }
        }
    }
    return best;
}

std::string AssetDatabase::relKey(const fs::path& absPath, int sourceIndex) const {
    std::error_code ec;
    fs::path rel = fs::relative(absPath, m_sources[sourceIndex].root, ec);
    if (ec || rel.empty()) rel = absPath.filename();
    return rel.generic_string();
}

void AssetDatabase::loadOrCreateMeta(Entry& e) {
    const fs::path metaPath = metaPathFor(e.absPath);

    if (std::ifstream in{metaPath}) {
        json j;
        bool parsed = false;
        try {
            in >> j;
            parsed = true;
        } catch (const json::exception&) {
            parsed = false; // corrupt sidecar -> regenerate
        }
        if (parsed) {
            e.id = AssetId::fromString(j.value("guid", std::string{}));
            if (e.type == AssetType::Texture) {
                const json imp = j.value("importer", json::object());
                e.textureImport.flipVertically =
                    imp.value("flipVertically", e.textureImport.flipVertically);
                e.textureImport.sRGB = imp.value("sRGB", e.textureImport.sRGB);
            }
            if (e.id.valid()) return;
        }
    }

    // No usable sidecar: mint an id and write one.
    e.id = AssetId::generate();
    json j;
    j["guid"] = e.id.toString();
    j["type"] = assetTypeName(e.type);
    if (e.type == AssetType::Texture) {
        j["importer"] = {{"flipVertically", e.textureImport.flipVertically},
                         {"sRGB", e.textureImport.sRGB}};
    }
    if (std::ofstream out{metaPath}) {
        out << j.dump(2) << '\n';
    } else {
        std::fprintf(stderr, "[Fitzel] could not write asset meta '%s'\n",
                     metaPath.string().c_str());
    }
}

AssetId AssetDatabase::importFile(const fs::path& absPath, int sourceIndex) {
    const AssetType type = assetTypeForExtension(lowerExt(absPath));
    if (type == AssetType::Unknown || sourceIndex < 0) return {};

    const std::string key = absKey(absPath);
    if (auto it = m_byPath.find(key); it != m_byPath.end())
        return it->second; // already registered

    Entry e;
    e.absPath     = canonicalPath(absPath);
    e.relPath     = relKey(e.absPath, sourceIndex);
    e.type        = type;
    e.sourceIndex = sourceIndex;
    loadOrCreateMeta(e);
    if (!e.id.valid()) return {};

    const AssetId id = e.id;
    m_byPath.emplace(key, id);
    m_byId.emplace(id, std::move(e));
    m_order.push_back(id);
    return id;
}

void AssetDatabase::refresh() {
    // Rebuild the registry from disk; ids come back stable from the sidecars.
    // The weak decode caches are keyed by id and intentionally kept.
    m_byId.clear();
    m_byPath.clear();
    m_order.clear();

    for (int si = 0; si < static_cast<int>(m_sources.size()); ++si) {
        const Source& src = m_sources[si];
        std::error_code ec;
        if (!fs::exists(src.root, ec)) {
            std::fprintf(stderr, "[Fitzel] %s asset source '%s' root '%s' missing\n",
                         sourceKindName(src.kind), src.name.c_str(),
                         src.root.string().c_str());
            continue;
        }
        for (fs::recursive_directory_iterator it(src.root, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            const fs::directory_entry& de = *it;
            if (!de.is_regular_file(ec)) continue;
            const fs::path& p = de.path();
            if (p.extension() == ".meta") continue; // sidecars are not assets
            importFile(p, si);
        }
    }
}

AssetId AssetDatabase::idForPath(const fs::path& path) {
    // Resolve relative paths against each source root until one contains it.
    fs::path abs = path;
    if (!abs.is_absolute()) {
        for (const Source& s : m_sources) {
            std::error_code ec;
            if (fs::exists(s.root / path, ec)) { abs = s.root / path; break; }
        }
        if (!abs.is_absolute()) abs = path; // fall through; may still be invalid
    }
    abs = canonicalPath(abs);

    if (auto it = m_byPath.find(abs.generic_string()); it != m_byPath.end())
        return it->second;

    const int si = sourceIndexForPath(abs);
    std::error_code ec;
    if (si >= 0 && fs::exists(abs, ec)) return importFile(abs, si);
    return {};
}

fs::path AssetDatabase::pathForId(AssetId id) const {
    if (auto it = m_byId.find(id); it != m_byId.end()) return it->second.absPath;
    return {};
}

AssetType AssetDatabase::typeForId(AssetId id) const {
    if (auto it = m_byId.find(id); it != m_byId.end()) return it->second.type;
    return AssetType::Unknown;
}

AssetSourceKind AssetDatabase::sourceKindForId(AssetId id) const {
    if (auto it = m_byId.find(id); it != m_byId.end()) {
        const int si = it->second.sourceIndex;
        if (si >= 0 && si < static_cast<int>(m_sources.size()))
            return m_sources[si].kind;
    }
    return AssetSourceKind::Engine;
}

const AssetDatabase::Entry* AssetDatabase::entry(AssetId id) const {
    if (auto it = m_byId.find(id); it != m_byId.end()) return &it->second;
    return nullptr;
}

std::shared_ptr<Texture> AssetDatabase::loadTexture(AssetId id) {
    const Entry* e = entry(id);
    if (!e || e->type != AssetType::Texture) return nullptr;

    if (auto it = m_texCache.find(id); it != m_texCache.end()) {
        if (auto sp = it->second.lock()) return sp; // cache hit -> dedup
    }

    Texture tex = Texture::fromFile(e->absPath.string(), e->textureImport.flipVertically);
    if (!tex.isValid()) {
        std::fprintf(stderr, "[Fitzel] failed to load texture '%s'\n",
                     e->absPath.string().c_str());
        return nullptr;
    }
    auto sp = std::make_shared<Texture>(std::move(tex));
    m_texCache[id] = sp;
    return sp;
}

std::shared_ptr<ModelData> AssetDatabase::loadModelData(AssetId id) {
    const Entry* e = entry(id);
    if (!e || e->type != AssetType::Model) return nullptr;

    if (auto it = m_modelCache.find(id); it != m_modelCache.end()) {
        if (auto sp = it->second.lock()) return sp;
    }

    auto sp = std::make_shared<ModelData>(loadGltf(e->absPath.string()));
    if (sp->empty()) return nullptr; // loadGltf already logged
    m_modelCache[id] = sp;
    return sp;
}

std::shared_ptr<Texture> AssetDatabase::loadTexture(const fs::path& path) {
    return loadTexture(idForPath(path));
}

std::shared_ptr<ModelData> AssetDatabase::loadModelData(const fs::path& path) {
    return loadModelData(idForPath(path));
}

} // namespace fitzel
