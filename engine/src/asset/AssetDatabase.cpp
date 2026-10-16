#include "fitzel/asset/AssetDatabase.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "fitzel/asset/Vfs.hpp"
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

// Load a model file, routing by extension: .dae -> Collada (assimp), else glTF.
ModelData loadModelFile(const fs::path& p) {
    const std::string ext = lowerExt(p);
    if (ext == "dae") return loadCollada(p.string());
    // FBX is where rigged characters arrive, so it goes through the assimp loader
    // that extracts a skeleton (and falls back to static output when there is no
    // animation in the file).
    if (ext == "fbx") return loadSkinnedModel(p.string());
    return loadGltf(p.string());
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

void AssetDatabase::unmountProjects() {
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(),
                        [](const Source& s) {
                            return s.kind == AssetSourceKind::Project;
                        }),
                    m_sources.end());
}

int AssetDatabase::mountProject(fs::path root) {
    unmountProjects(); // keep exactly one project mounted at a time
    return mount("project", AssetSourceKind::Project, std::move(root));
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

namespace {
// A 32-hex-digit GUID anywhere in a damaged sidecar, after the "guid" key:
// what is worth rescuing from a .meta whose JSON no longer parses (a merge
// conflict marker, a truncated write) -- every scene reference hangs on it.
AssetId salvageGuid(const std::string& body) {
    const std::size_t key = body.find("\"guid\"");
    if (key == std::string::npos) return {};
    std::size_t at = body.find('"', body.find(':', key + 6) + 1);
    if (at == std::string::npos) return {};
    return AssetId::fromString(std::string_view(body).substr(at + 1, 32));
}

// Write a file whole or not at all: to a temp name, then renamed over the old
// one, so a crash or a full disk mid-write cannot leave half a sidecar.
bool writeAtomically(const fs::path& path, const std::string& text) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        out.close();
        if (out.fail()) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {   // not every Windows filesystem renames over an existing file
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) fs::remove(tmp, ec);
    return !ec;
}
} // namespace

void AssetDatabase::loadOrCreateMeta(Entry& e) {
    const fs::path metaPath = metaPathFor(e.absPath);

    // What an unusable sidecar still had in it: its GUID if one can be found,
    // and every key it carried (a newer editor's settings, say), kept when the
    // sidecar is written again rather than thrown away with the rest.
    json    keep = json::object();
    AssetId rescued;
    bool    damaged = false;
    if (const std::string body = vfs::readText(metaPath.generic_string());
        !body.empty()) {
        json j = json::parse(body, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            // Every field read by type: value() throws on a wrong one (a guid
            // written as a number), and one bad sidecar used to abort the
            // whole refresh.
            const auto guid = j.find("guid");
            if (guid != j.end() && guid->is_string())
                e.id = AssetId::fromString(guid->get<std::string>());
            if (e.type == AssetType::Texture) {
                const auto imp = j.find("importer");
                if (imp != j.end() && imp->is_object()) {
                    const auto flip = imp->find("flipVertically");
                    if (flip != imp->end() && flip->is_boolean())
                        e.textureImport.flipVertically = flip->get<bool>();
                    const auto srgb = imp->find("sRGB");
                    if (srgb != imp->end() && srgb->is_boolean())
                        e.textureImport.sRGB = srgb->get<bool>();
                }
            }
            if (e.id.valid()) return;
            keep = std::move(j);
        } else {
            rescued = salvageGuid(body);
        }
        damaged = true;
        // Kept beside it before anything is written: whatever was in there is
        // somebody's, and a GUID that could not be rescued can still be put
        // back by hand from this copy.
        if (!vfs::packed()) {
            fs::path backup = metaPath;
            backup += ".corrupt";
            std::error_code ec;
            fs::copy_file(metaPath, backup, fs::copy_options::overwrite_existing, ec);
            std::fprintf(stderr, "[Fitzel] asset meta '%s' is damaged -- %s; the old one is "
                                 "kept as '%s'\n",
                         metaPath.string().c_str(),
                         rescued.valid() ? "its GUID was rescued"
                                         : "a NEW GUID is given, references to the old break",
                         backup.filename().string().c_str());
        }
    }

    // No usable sidecar: the rescued id, else a new one, and write it.
    e.id = rescued.valid() ? rescued : AssetId::generate();
    // ...except in a packed build, where there is nowhere to write and a minted
    // id is worse than useless: it is fresh on every launch, so every scene
    // reference to this asset dangles. The sidecars are packed with the content
    // precisely so the ids survive the export -- one missing means the export
    // dropped it, and that is worth saying out loud.
    if (vfs::packed()) {
        std::fprintf(stderr, "[Fitzel] %s .meta for '%s' in the archive -- "
                             "references to it will not resolve\n",
                     damaged ? "damaged" : "no", e.relPath.c_str());
        return;
    }
    json j = std::move(keep);
    j["guid"] = e.id.toString();
    j["type"] = assetTypeName(e.type);
    if (e.type == AssetType::Texture) {
        j["importer"] = {{"flipVertically", e.textureImport.flipVertically},
                         {"sRGB", e.textureImport.sRGB}};
    }
    if (!writeAtomically(metaPath, j.dump(2) + '\n'))
        std::fprintf(stderr, "[Fitzel] could not write asset meta '%s'\n",
                     metaPath.string().c_str());
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
    std::error_code mec;
    e.mtime       = fs::last_write_time(e.absPath, mec);
    loadOrCreateMeta(e);
    if (!e.id.valid()) return {};

    const AssetId id = e.id;
    // Two files claiming one GUID (a copied .meta, or ids minted twice): the
    // first keeps it. Listing the id again would only show that first file twice
    // and this one never, so say which files clash instead.
    if (auto it = m_byId.find(id); it != m_byId.end()) {
        std::fprintf(stderr, "[Fitzel] '%s' has the same GUID as '%s' -- delete its "
                             ".meta to give it a new one\n",
                     e.absPath.string().c_str(), it->second.absPath.string().c_str());
        return {};
    }
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
        if (!vfs::isDirectory(src.root.generic_string())) {
            std::fprintf(stderr, "[Fitzel] %s asset source '%s' root '%s' missing\n",
                         sourceKindName(src.kind), src.name.c_str(),
                         src.root.string().c_str());
            continue;
        }
        // Through the VFS, so a packed build enumerates the archive's index
        // instead of a content/ folder that isn't there.
        for (const std::string& f : vfs::listFiles(src.root.generic_string(), true)) {
            const fs::path p(f);
            if (p.extension() == ".meta") continue; // sidecars are not assets
            importFile(p, si);
        }
    }
}

AssetId AssetDatabase::idForPath(const fs::path& path) {
    // Resolve relative paths against each source root until one contains it.
    fs::path abs = path;
    if (!abs.is_absolute()) {
        for (const Source& s : m_sources)
            if (vfs::exists((s.root / path).generic_string())) {
                abs = s.root / path;
                break;
            }
        if (!abs.is_absolute()) abs = path; // fall through; may still be invalid
    }
    abs = canonicalPath(abs);

    if (auto it = m_byPath.find(abs.generic_string()); it != m_byPath.end())
        return it->second;

    const int si = sourceIndexForPath(abs);
    if (si >= 0 && vfs::exists(abs.generic_string())) return importFile(abs, si);
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

std::shared_ptr<Texture> AssetDatabase::loadTextureForModel(AssetId id) {
    const Entry* e = entry(id);
    if (!e || e->type != AssetType::Texture) return nullptr;

    if (auto it = m_modelTexCache.find(id); it != m_modelTexCache.end()) {
        if (auto sp = it->second.lock()) return sp;
    }

    Texture tex = Texture::fromFile(e->absPath.string(), /*flipVertically=*/false);
    if (!tex.isValid()) {
        std::fprintf(stderr, "[Fitzel] failed to load texture '%s'\n",
                     e->absPath.string().c_str());
        return nullptr;
    }
    auto sp = std::make_shared<Texture>(std::move(tex));
    m_modelTexCache[id] = sp;
    return sp;
}

std::shared_ptr<ModelData> AssetDatabase::loadModelData(AssetId id) {
    const Entry* e = entry(id);
    if (!e || e->type != AssetType::Model) return nullptr;

    if (auto it = m_modelCache.find(id); it != m_modelCache.end()) {
        if (auto sp = it->second.lock()) return sp;
    }

    auto sp = std::make_shared<ModelData>(loadModelFile(e->absPath));
    if (sp->empty()) return nullptr; // the loader already logged
    m_modelCache[id] = sp;
    return sp;
}

std::shared_ptr<Texture> AssetDatabase::loadTexture(const fs::path& path) {
    return loadTexture(idForPath(path));
}

std::shared_ptr<ModelData> AssetDatabase::loadModelData(const fs::path& path) {
    return loadModelData(idForPath(path));
}

void AssetDatabase::reloadInPlace(const Entry& e) {
    // Only reload assets that are still held by someone (live weak cache);
    // otherwise the next load() re-reads from disk anyway. Reload into the same
    // object so existing shared handles observe the change without being rebound.
    if (e.type == AssetType::Texture) {
        if (auto it = m_texCache.find(e.id); it != m_texCache.end())
            if (auto sp = it->second.lock()) {
                Texture t = Texture::fromFile(e.absPath.string(),
                                              e.textureImport.flipVertically);
                if (t.isValid()) *sp = std::move(t); // keep old pixels on a bad read
            }
        // ...and the copy a model samples, if one is alive (see
        // loadTextureForModel). Checked on its own: either may be held alone.
        if (auto mt = m_modelTexCache.find(e.id); mt != m_modelTexCache.end())
            if (auto sp = mt->second.lock()) {
                Texture t = Texture::fromFile(e.absPath.string(), false);
                if (t.isValid()) *sp = std::move(t);
            }
    } else if (e.type == AssetType::Model) {
        auto it = m_modelCache.find(e.id);
        if (it == m_modelCache.end()) return;
        if (auto sp = it->second.lock()) {
            ModelData md = loadModelFile(e.absPath);
            if (!md.empty()) *sp = std::move(md);
        }
    }
}

std::vector<AssetChange> AssetDatabase::pollChanges() {
    std::vector<AssetChange> changes;
    // A mounted archive cannot change under us, and walking its index a couple of
    // times a second to establish that would be pure waste.
    if (vfs::packed()) return changes;
    std::error_code ec;
    std::unordered_set<std::string> seen; // absolute keys present on disk now
    std::unordered_map<std::string, std::string> walkKeys;
    walkKeys.reserve(m_walkKeys.size());

    for (int si = 0; si < static_cast<int>(m_sources.size()); ++si) {
        const Source& src = m_sources[si];
        if (!fs::exists(src.root, ec)) continue;
        for (fs::recursive_directory_iterator it(src.root, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const fs::path& p = it->path();
            if (p.extension() == ".meta") continue;
            if (assetTypeForExtension(lowerExt(p)) == AssetType::Unknown) continue;

            std::string walk = p.generic_string();
            const auto known = m_walkKeys.find(walk);
            const std::string key = known != m_walkKeys.end() ? known->second : absKey(p);
            walkKeys.emplace(std::move(walk), key);
            seen.insert(key);
            auto pit = m_byPath.find(key);
            if (pit == m_byPath.end()) {
                const AssetId id = importFile(p, si); // new file
                if (id.valid())
                    changes.push_back({id, AssetChange::Kind::Added, typeForId(id)});
                continue;
            }
            Entry& e = m_byId.at(pit->second);
            std::error_code mec;
            const auto mt = fs::last_write_time(p, mec);
            if (!mec && mt != e.mtime) {
                e.mtime = mt;
                reloadInPlace(e);
                changes.push_back({e.id, AssetChange::Kind::Modified, e.type});
            }
        }
    }

    m_walkKeys = std::move(walkKeys);

    // Anything registered but no longer on disk was removed.
    std::unordered_set<AssetId> gone;
    for (const auto& [id, e] : m_byId)
        if (!seen.count(e.absPath.generic_string())) gone.insert(id);
    if (gone.empty()) return changes;
    for (const AssetId id : gone) {
        const Entry& e = m_byId.at(id);
        changes.push_back({id, AssetChange::Kind::Removed, e.type});
        m_byPath.erase(e.absPath.generic_string());
        m_texCache.erase(id);
        m_modelTexCache.erase(id);
        m_modelCache.erase(id);
        m_byId.erase(id);
    }
    // One pass over the listing for all of them, not one per removed asset.
    m_order.erase(std::remove_if(m_order.begin(), m_order.end(),
                                 [&](const AssetId& id) { return gone.count(id) != 0; }),
                  m_order.end());
    return changes;
}

} // namespace fitzel
