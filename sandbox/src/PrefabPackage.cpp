#include "PrefabPackage.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "miniz.h"

#include <fitzel/Version.hpp>
#include <fitzel/asset/AssetDatabase.hpp>

#include "AnimGraph.hpp"

namespace prefabpkg {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// The package's table of contents, at the top of the zip.
constexpr const char* kManifest = "fitzel-prefab.json";
constexpr int         kFormat   = 1;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// A 32-hex-digit string is an AssetId: how prefabs name models and materials.
bool looksLikeGuid(const std::string& s) {
    if (s.size() != 32) return false;
    for (unsigned char c : s) if (!std::isxdigit(c)) return false;
    return true;
}

std::string extOf(const std::string& name) {
    return lower(fs::path(name).extension().string());
}

bool isTextureExt(const std::string& e) {
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" ||
           e == ".exr" || e == ".hdr";
}

// What the asset database knows as an asset (AssetTypes.cpp) -- the files a
// component can name by file name and have the engine find by that name.
bool isAssetExt(const std::string& e) {
    return isTextureExt(e) || e == ".gltf" || e == ".glb" || e == ".dae" || e == ".fbx" ||
           e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac" || e == ".fmat" ||
           e == ".fvid";
}

bool readBytes(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeBytes(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// `abs` as a '/'-separated path inside `root`, or "" when it lies outside.
std::string relInside(const fs::path& root, const fs::path& abs) {
    std::error_code ec;
    const fs::path r = fs::relative(fs::weakly_canonical(abs, ec),
                                    fs::weakly_canonical(root, ec), ec);
    const std::string s = r.generic_string();
    if (ec || s.empty() || s == "." || s.rfind("..", 0) == 0) return {};
    return s;
}

// A zip entry that stays inside the project it is unpacked into: relative,
// forward slashes, no step up, no drive letter. A package with anything else is
// trying to write somewhere nobody asked it to, and is refused whole.
bool safeEntry(const std::string& e) {
    if (e.empty() || e.front() == '/' || e.find('\\') != std::string::npos ||
        e.find(':') != std::string::npos)
        return false;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = e.find('/', start);
        const std::string part = e.substr(start, end == std::string::npos
                                                     ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
}

// Every string in a JSON tree with the key it sits under (an array's elements
// under the array's key).
template <class F>
void eachString(const json& j, const std::string& key, F&& f) {
    if (j.is_string()) {
        f(key, j.get<std::string>());
    } else if (j.is_array()) {
        for (const auto& e : j) eachString(e, key, f);
    } else if (j.is_object()) {
        for (const auto& it : j.items()) eachString(it.value(), it.key(), f);
    }
}

template <class F>
void rewriteStrings(json& j, F&& f) {
    if (j.is_string()) j = f(j.get<std::string>());
    else if (j.is_array() || j.is_object()) for (auto& e : j) rewriteStrings(e, f);
}

// A glider's sounds are a comma-separated list; everything else is one name.
std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s + ",") {
        if (c != ',') { cur += c; continue; }
        const std::string t = trim(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
    }
    return out;
}

// Quoted string literals in Lua source, both quote styles. Best effort: what a
// script names -- a sound it plays, a prefab it spawns -- has to come along.
std::vector<std::string> luaStrings(const std::string& src) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char q = src[i];
        if (q != '"' && q != '\'') continue;
        std::string s;
        for (++i; i < src.size() && src[i] != q && src[i] != '\n'; ++i) {
            if (src[i] == '\\' && i + 1 < src.size()) s.push_back(src[++i]);
            else s.push_back(src[i]);
        }
        out.push_back(s);
    }
    return out;
}

// The texture file names a binary or text model (FBX, DAE) mentions: whatever
// runs of printable characters end in an image extension. An FBX names its maps
// inside itself, not by GUID; this finds them without an FBX parser.
std::vector<std::string> namedImages(const std::string& bytes) {
    std::set<std::string> found;
    const std::string low = lower(bytes);
    for (const char* ext : {".png", ".jpg", ".jpeg", ".tga", ".bmp"}) {
        const std::size_t n = std::strlen(ext);
        for (std::size_t at = low.find(ext); at != std::string::npos; at = low.find(ext, at + 1)) {
            const std::size_t end = at + n;
            if (end < low.size() && std::isalnum(static_cast<unsigned char>(low[end]))) continue;
            std::size_t start = at;
            while (start > 0) {
                const unsigned char c = static_cast<unsigned char>(bytes[start - 1]);
                if (c < 32 || c > 126 || c == '"' || c == '<' || c == '>' || c == '/' ||
                    c == '\\')
                    break;
                --start;
            }
            if (start < at) found.insert(bytes.substr(start, end - start));
        }
    }
    return {found.begin(), found.end()};
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// A prefab's display name -> its file, over a project's prefabs/ folder.
std::unordered_map<std::string, fs::path> prefabsByName(const fs::path& proj) {
    std::unordered_map<std::string, fs::path> out;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(proj / "prefabs", ec)) {
        if (de.path().extension() != ".fprefab") continue;
        std::string body;
        if (!readBytes(de.path(), body)) continue;
        try {
            const json j = json::parse(body);
            out.emplace(lower(j.at("prefab").value("name", de.path().stem().string())), de.path());
        } catch (const json::exception&) {}
    }
    return out;
}

// --- Export ---------------------------------------------------------------------
struct Packer {
    const ExportSource& src;
    fs::path            proj;
    Report&             rep;

    std::map<std::string, fs::path>    files;     // zip entry -> file on disk
    std::map<std::string, std::string> rewritten; // zip entry -> bytes, for prefabs
    std::set<std::string>              noted;
    std::set<std::string>              prefabsDone;
    std::unordered_map<std::string, fs::path> projectByName; // lower file name -> asset
    std::set<std::string>                     engineByName;  // lower file names
    std::unordered_map<std::string, fs::path> prefabNames;
    std::vector<fs::path>                     engineRoots;
    bool                                      sawModel = false;

    Packer(const ExportSource& s, Report& r) : src(s), proj(s.projectFolder), rep(r) {
        for (const auto& so : src.assetDb->sources())
            if (so.kind == fitzel::AssetSourceKind::Engine) engineRoots.push_back(so.root);
        for (const fitzel::AssetId id : src.assetDb->allAssets()) {
            const auto* e = src.assetDb->entry(id);
            if (!e) continue;
            const std::string key = lower(e->absPath.filename().string());
            if (src.assetDb->sourceKindForId(id) == fitzel::AssetSourceKind::Engine)
                engineByName.insert(key);
            else
                projectByName.emplace(key, e->absPath);
        }
        prefabNames = prefabsByName(proj);
    }

    void note(const std::string& s) {
        if (noted.insert(s).second) rep.notes.push_back(s);
    }

    bool inEngine(const fs::path& abs) const {
        for (const fs::path& r : engineRoots)
            if (!relInside(r, abs).empty()) return true;
        return false;
    }

    // A file into the zip at its place in the project, with its .meta.
    bool addFile(const fs::path& abs) {
        const std::string rel = relInside(proj, abs);
        if (rel.empty() || !files.emplace(rel, abs).second) return false;
        fs::path meta = abs;
        meta += ".meta";
        std::error_code ec;
        if (fs::exists(meta, ec)) files.emplace(rel + ".meta", meta);
        return true;
    }

    // ...and, for the kinds that name further files, those as well.
    void addAsset(const fs::path& abs) {
        std::error_code ec;
        if (!fs::exists(abs, ec)) return;
        if (relInside(proj, abs).empty()) {
            note(inEngine(abs)
                     ? "Engine content, not packed (every fitzel has it): " +
                           abs.filename().string()
                     : "Outside the project folder, not packed: " + abs.generic_string());
            return;
        }
        if (!addFile(abs)) return;
        const std::string ext = extOf(abs.filename().string());
        std::string body;
        if (ext == ".fmat" && readBytes(abs, body)) {
            try { eachString(json::parse(body), "", [&](const std::string& k, const std::string& v) { token(k, v); }); }
            catch (const json::exception&) {}
        } else if (ext == ".lua" && readBytes(abs, body)) {
            for (const std::string& s : luaStrings(body)) {
                if (prefabNames.count(lower(s))) nestedPrefab(s);
                else for (const std::string& part : splitList(s)) fileName(part, false);
            }
        } else if (ext == ".gltf") {
            gltfCompanions(abs);
        } else if (ext == ".fbx" || ext == ".dae") {
            modelTextures(abs);
        }
        if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".dae") sawModel = true;
    }

    void byGuid(const std::string& v) {
        const fitzel::AssetId id = fitzel::AssetId::fromString(v);
        if (!id.valid()) return;
        const fs::path p = src.assetDb->pathForId(id);
        if (!p.empty()) addAsset(p);
    }

    // A name the engine finds by file name: a sound, a sprite, a script.
    void fileName(const std::string& name, bool complain) {
        const std::string ext = extOf(name);
        if (ext.empty()) return;
        std::error_code ec;
        if (ext == ".lua") {
            const fs::path p = proj / "scripts" / name;
            if (fs::exists(p, ec)) addAsset(p);
            else if (complain) note("Script not found in scripts/, not packed: " + name);
            return;
        }
        if (ext == ".mid" || ext == ".midi") { synthFile(name, "midi", ".mid"); return; }
        if (!isAssetExt(ext)) return;
        // A path relative to the project (a track entry's image) first, then the
        // name the way the engine looks a sound or sprite up: any asset called that.
        if (fs::exists(proj / name, ec) && !relInside(proj, proj / name).empty()) {
            addAsset(proj / name);
            return;
        }
        const std::string key = lower(fs::path(name).filename().string());
        if (const auto it = projectByName.find(key); it != projectByName.end()) {
            addAsset(it->second);
        } else if (engineByName.count(key)) {
            note("Engine content, not packed (every fitzel has it): " + name);
        } else if (complain) {
            note("Not found in the project, not packed: " + name);
        }
    }

    // A synth patch or MIDI file, found the way SynthSystem finds it.
    void synthFile(const std::string& name, const char* sub, const char* ext) {
        std::error_code ec;
        for (const fs::path& p : {proj / "content" / sub / name,
                                  proj / "content" / sub / (name + ext),
                                  proj / "content" / name, proj / name}) {
            if (fs::is_regular_file(p, ec)) { addFile(p); return; }
        }
        note(std::string(sub == std::string("midi") ? "MIDI file" : "Synth patch") +
             " not found in the project, not packed: " + name);
    }

    void token(const std::string& key, const std::string& v) {
        if (v.empty() || key == "type") return;
        if (looksLikeGuid(v)) { byGuid(v); return; }
        if (key == "prefab") { nestedPrefab(v); return; }
        if (key == "scene") {
            note("Scene \"" + v + "\" named by a component is not packed: scenes stay in their project.");
            return;
        }
        if (key == "patch") { synthFile(v, "patches", ".json"); return; }
        if (key == "midi")  { synthFile(v, "midi", ".mid"); return; }
        for (const std::string& part : splitList(v)) {
            if (looksLikeGuid(part)) byGuid(part);
            else fileName(part, true);
        }
        // A modelled mesh keeps its per-face materials as one string of numbers
        // and GUIDs.
        if (v.find(' ') != std::string::npos) {
            std::string word;
            for (const char c : v + " ") {
                if (c != ' ') { word += c; continue; }
                if (looksLikeGuid(word)) byGuid(word);
                word.clear();
            }
        }
    }

    void nestedPrefab(const std::string& name) {
        const auto it = prefabNames.find(lower(name));
        if (it == prefabNames.end()) {
            note("Prefab \"" + name + "\" named by a component is not in this project, not packed.");
            return;
        }
        std::string err;
        packPrefab(it->second, err);
    }

    // A glTF names its buffers and images by relative URI.
    void gltfCompanions(const fs::path& abs) {
        std::string body;
        if (!readBytes(abs, body)) return;
        try {
            const json j = json::parse(body);
            for (const char* list : {"buffers", "images"})
                if (j.contains(list))
                    for (const auto& b : j[list]) {
                        const std::string uri = b.value("uri", std::string());
                        if (uri.empty() || uri.rfind("data:", 0) == 0) continue;
                        addAsset(abs.parent_path() / fs::path(percentDecode(uri)));
                    }
        } catch (const json::exception&) {}
    }

    // An FBX or DAE finds its maps by the names inside it, beside itself or in a
    // textures folder near it (Model.cpp's findTextureFile). Found the same way
    // here; names that match nothing were most likely packed into the model.
    void modelTextures(const fs::path& abs) {
        std::string body;
        if (!readBytes(abs, body)) return;
        const fs::path dir = abs.parent_path();
        std::vector<fs::path> where{dir};
        for (const fs::path& base : {dir, dir.parent_path()})
            for (const char* sub : {"Textures", "Texture", "textures", "Tex", "Materials", "Maps"})
                where.push_back(base / sub);
        std::error_code ec;
        for (const std::string& named : namedImages(body)) {
            const std::string file = fs::path(named).filename().string();
            for (const fs::path& w : where)
                if (fs::is_regular_file(w / file, ec)) { addAsset(w / file); break; }
        }
        // Siblings sharing the model's name (a .mtl, say) ride along too.
        for (const auto& sib : fs::directory_iterator(dir, ec))
            if (sib.is_regular_file() && sib.path() != abs && sib.path().stem() == abs.stem() &&
                sib.path().extension() != ".meta")
                addFile(sib.path());
    }

    // The prefab itself, with the graphs its objects run written into it.
    bool packPrefab(const fs::path& path, std::string& err) {
        std::string rel = relInside(proj, path);
        if (rel.empty()) rel = "prefabs/" + path.filename().string();
        if (!prefabsDone.insert(rel).second) return true;
        std::string body;
        json j;
        if (!readBytes(path, body)) { err = "Cannot read " + path.generic_string(); return false; }
        try { j = json::parse(body); }
        catch (const json::exception&) { err = "Not a readable prefab: " + path.generic_string(); return false; }
        if (!j.contains("prefab") || !j["prefab"].is_object()) {
            err = "Not a prefab: " + path.generic_string();
            return false;
        }
        json& pj = j["prefab"];

        std::vector<std::string> graphNames;
        for (const auto& e : pj.value("entities", json::array()))
            for (const auto& c : e.value("components", json::array())) {
                const std::string type = c.value("type", std::string());
                if (type == "anim_graph") {
                    const std::string g = c.value("graph", std::string());
                    if (!g.empty()) graphNames.push_back(g);
                    continue;   // a graph's name is not a file
                }
                if (type == "animator" && !c.value("clip", std::string()).empty())
                    note("Animator clip \"" + c.value("clip", std::string()) +
                         "\" is a Timeline clip: those stay in their scene.");
                eachString(c, "", [&](const std::string& k, const std::string& v) { token(k, v); });
            }

        // What it carries already, topped up from the open scene for a prefab
        // saved before graphs travelled with it.
        std::vector<animgraph::Graph> carried;
        if (pj.contains("animGraphs"))
            animgraph::load(json{{"animGraphs", pj["animGraphs"]}}, carried);
        for (const std::string& name : graphNames) {
            if (animgraph::findGraph(carried, name) >= 0) continue;
            const int gi = src.sceneGraphs ? animgraph::findGraph(*src.sceneGraphs, name) : -1;
            if (gi >= 0) carried.push_back((*src.sceneGraphs)[static_cast<std::size_t>(gi)]);
            else note("Animation graph \"" + name + "\" is not in the open scene, not packed -- "
                      "open the scene the object comes from and export again.");
        }
        for (const animgraph::Graph& g : carried)
            for (const animgraph::State& st : g.states)
                if (!st.clip.empty())
                    note("Graph \"" + g.name + "\", state \"" + st.name + "\" plays Timeline clip \"" +
                         st.clip + "\": Timeline clips stay in their scene.");
        if (!carried.empty()) {
            json gj;
            animgraph::save(gj, carried);
            pj["animGraphs"] = gj["animGraphs"];
        }
        files[rel]     = path;
        rewritten[rel] = j.dump(2) + "\n";
        return true;
    }
};

} // namespace

bool exportZip(const ExportSource& src, const std::string& prefabPath,
               const std::string& zipPath, Report& rep, std::string& err) {
    rep = {};
    if (!src.assetDb || src.projectFolder.empty()) { err = "No project is open."; return false; }
    Packer pk(src, rep);
    if (!pk.packPrefab(fs::path(prefabPath), err)) return false;
    if (pk.sawModel)
        pk.note("Changes made in the Materials panel to a model's OWN materials are scene "
                "settings and stay behind; library materials travel.");

    json manifest;
    std::string mainEntry = relInside(pk.proj, fs::path(prefabPath));
    if (mainEntry.empty()) mainEntry = "prefabs/" + fs::path(prefabPath).filename().string();
    const json mainJson = json::parse(pk.rewritten[mainEntry])["prefab"];
    manifest["format"] = kFormat;
    manifest["app"]    = fitzel::kVersionFull;
    manifest["prefab"] = {{"name", mainJson.value("name", std::string())},
                          {"guid", mainJson.value("guid", std::string())},
                          {"file", mainEntry}};
    json list = json::array();
    for (const auto& f : pk.files) {
        list.push_back(f.first);
        rep.files.push_back(f.first);
    }
    manifest["files"] = std::move(list);
    manifest["notes"] = rep.notes;

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) { err = "Cannot start the zip."; return false; }
    bool ok = true;
    auto add = [&](const std::string& name, const std::string& bytes) {
        if (ok && !mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                                         MZ_DEFAULT_COMPRESSION)) {
            err = "Cannot add " + name + " to the zip.";
            ok = false;
        }
    };
    add(kManifest, manifest.dump(2) + "\n");
    for (const auto& f : pk.files) {
        if (const auto it = pk.rewritten.find(f.first); it != pk.rewritten.end()) {
            add(f.first, it->second);
            continue;
        }
        std::string bytes;
        if (!readBytes(f.second, bytes)) {
            err = "Cannot read " + f.second.generic_string();
            ok = false;
            break;
        }
        add(f.first, bytes);
    }
    void*       buf  = nullptr;
    std::size_t size = 0;
    if (ok && !mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        err = "Cannot finish the zip.";
        ok = false;
    }
    mz_zip_writer_end(&zip);
    if (ok && !writeBytes(fs::path(zipPath), std::string(static_cast<const char*>(buf), size))) {
        err = "Cannot write " + zipPath;
        ok = false;
    }
    if (buf) mz_free(buf);
    return ok;
}
// --- Import ----------------------------------------------------------------------

namespace {

// A zip read into memory, its entries by name. Loading refuses a package with an
// entry that would land outside the project.
struct ZipIn {
    std::string                    bytes;
    mz_zip_archive                 zip;
    std::map<std::string, mz_uint> index;
    bool                           open = false;

    ZipIn() { std::memset(&zip, 0, sizeof zip); }
    ~ZipIn() { if (open) mz_zip_reader_end(&zip); }
    ZipIn(const ZipIn&) = delete;
    ZipIn& operator=(const ZipIn&) = delete;

    bool load(const std::string& path, std::string& err) {
        if (!readBytes(fs::path(path), bytes)) { err = "Cannot read " + path; return false; }
        if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
            err = "Not a zip file: " + path;
            return false;
        }
        open = true;
        const mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            const std::string name = st.m_filename;
            if (!safeEntry(name)) {
                err = "The package would write outside the project (\"" + name + "\") -- refused.";
                return false;
            }
            index[name] = i;
        }
        return true;
    }
    bool has(const std::string& name) const { return index.count(name) != 0; }
    std::string read(const std::string& name) {
        const auto it = index.find(name);
        if (it == index.end()) return {};
        std::size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, it->second, &size, 0);
        if (!p) return {};
        std::string out(static_cast<const char*>(p), size);
        mz_free(p);
        return out;
    }
};

std::string guidIn(const std::string& metaBytes) {
    try {
        const std::string g = json::parse(metaBytes).value("guid", std::string());
        return looksLikeGuid(g) ? lower(g) : std::string();
    } catch (const json::exception&) { return {}; }
}

std::string diskGuid(const fs::path& asset) {
    std::string body;
    return readBytes(fs::path(asset.string() + ".meta"), body) ? guidIn(body) : std::string();
}

// A material as content: without the id it carries (the GUID that counts is the
// .meta's) -- two projects' "Default" are the same material under two GUIDs.
json materialContent(json m) {
    if (m.is_object()) m.erase("id");
    return m;
}

struct ProjectMaterial {
    std::string name, guid, rel;
    json        content;
};

std::vector<ProjectMaterial> projectMaterials(const fs::path& proj) {
    std::vector<ProjectMaterial> out;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(proj / "materials", ec)) {
        if (de.path().extension() != ".fmat") continue;
        std::string body;
        if (!readBytes(de.path(), body)) continue;
        try {
            const json m = json::parse(body);
            out.push_back({m.value("name", std::string()), diskGuid(de.path()),
                           relInside(proj, de.path()), materialContent(m)});
        } catch (const json::exception&) {}
    }
    return out;
}

// The rewrites a plan asks for, applied to one string.
std::string rewrite(const Plan& plan, const std::string& s) {
    std::string out = s;
    for (const auto& [from, to] : plan.guidRemap)
        for (std::size_t at = out.find(from); at != std::string::npos;
             at = out.find(from, at + to.size()))
            out.replace(at, from.size(), to);
    for (const auto& [from, to] : plan.renamedFiles) {
        if (out == from) { out = to; continue; }
        if (out.find(',') == std::string::npos) continue;
        std::string joined;
        for (const std::string& part : splitList(out))
            joined += (joined.empty() ? "" : ", ") + (part == from ? to : part);
        out = joined;
    }
    return out;
}

std::string rewriteJson(const Plan& plan, const std::string& bytes) {
    if (plan.guidRemap.empty() && plan.renamedFiles.empty()) return bytes;
    try {
        json j = json::parse(bytes);
        rewriteStrings(j, [&](const std::string& s) { return rewrite(plan, s); });
        return j.dump(2) + "\n";
    } catch (const json::exception&) {
        return bytes;
    }
}

// "name (2).ext", "name (3).ext" ... beside `rel`, free on disk and in the plan.
std::string freeName(const fs::path& proj, const std::string& rel,
                     const std::set<std::string>& taken) {
    const fs::path p(rel);
    const std::string dir  = p.parent_path().generic_string();
    const std::string stem = p.stem().string(), ext = p.extension().string();
    std::error_code ec;
    for (int n = 2;; ++n) {
        const std::string cand = (dir.empty() ? "" : dir + "/") + stem + " (" +
                                 std::to_string(n) + ")" + ext;
        if (!taken.count(cand) && !fs::exists(proj / cand, ec)) return cand;
    }
}

} // namespace

bool planImport(const ImportTarget& dst, const std::string& zipPath, Plan& plan,
                std::string& err) {
    plan = {};
    plan.zipPath = zipPath;
    if (!dst.assetDb || dst.projectFolder.empty()) { err = "Open a project first."; return false; }
    ZipIn z;
    if (!z.load(zipPath, err)) return false;
    json manifest;
    try { manifest = json::parse(z.read(kManifest)); }
    catch (const json::exception&) {
        err = "Not a fitzel prefab package: it has no " + std::string(kManifest) + ".";
        return false;
    }
    if (manifest.value("format", 0) > kFormat) {
        err = "This package was made by a newer fitzel -- update this one to import it.";
        return false;
    }
    const json pj = manifest.value("prefab", json::object());
    plan.prefabName  = pj.value("name", std::string("Prefab"));
    plan.prefabEntry = pj.value("file", std::string());
    if (!z.has(plan.prefabEntry)) { err = "The package holds no prefab."; return false; }
    for (const auto& n : manifest.value("notes", json::array()))
        if (n.is_string()) plan.notes.push_back(n.get<std::string>());

    const fs::path proj(dst.projectFolder);
    const std::vector<ProjectMaterial> mats = projectMaterials(proj);
    std::unordered_map<std::string, std::string> prefabByGuid;   // guid -> rel
    {
        std::error_code ec;
        for (const auto& de : fs::directory_iterator(proj / "prefabs", ec)) {
            if (de.path().extension() != ".fprefab") continue;
            std::string body;
            if (!readBytes(de.path(), body)) continue;
            try {
                const std::string g = json::parse(body).at("prefab").value("guid", std::string());
                if (!g.empty()) prefabByGuid.emplace(lower(g), relInside(proj, de.path()));
            } catch (const json::exception&) {}
        }
    }

    // Plain files first, then materials, then prefabs: each later kind is
    // compared with what the project has AFTER the rewrites the earlier ones
    // asked for, so importing the same package twice finds it all there.
    std::vector<std::string> order[3];
    for (const auto& e : z.index) {
        const std::string ext = extOf(e.first);
        if (e.first == kManifest || ext == ".meta") continue;   // a .meta rides with its asset
        order[ext == ".fprefab" ? 2 : ext == ".fmat" ? 1 : 0].push_back(e.first);
    }
    std::set<std::string> taken;
    bool renamedAny = false, hasScripts = false;
    for (const auto& group : order)
        for (const std::string& name : group) {
            Item it;
            it.entry  = name;
            it.target = name;
            const std::string ext  = extOf(name);
            hasScripts |= ext == ".lua";
            std::string bytes = z.read(name);
            if (ext == ".fprefab" || ext == ".fmat") bytes = rewriteJson(plan, bytes);
            const fs::path at = proj / name;
            std::error_code ec;
            std::string have;
            const bool exists = fs::exists(at, ec) || taken.count(name);

            if (ext == ".fprefab") {
                std::string guid;
                try { guid = lower(json::parse(bytes).at("prefab").value("guid", std::string())); }
                catch (const json::exception&) {}
                const auto same = guid.empty() ? prefabByGuid.end() : prefabByGuid.find(guid);
                if (same != prefabByGuid.end()) {
                    it.target = same->second;
                    readBytes(proj / it.target, have);
                    if (have == bytes) {
                        it.action = Item::Action::Present;
                        it.why    = "this prefab is already in the project";
                    } else {
                        it.action = Item::Action::Update;
                        it.why    = "an older version of this prefab is replaced (kept as .bak)";
                    }
                } else if (exists) {
                    it.action = Item::Action::Rename;
                    it.target = freeName(proj, name, taken);
                    it.why    = "a different prefab file of that name is already there";
                }
                taken.insert(it.target);
                plan.items.push_back(it);
                continue;
            }

            const std::string guid = z.has(name + ".meta") ? guidIn(z.read(name + ".meta"))
                                                           : std::string();
            if (!guid.empty()) {
                const fs::path p = dst.assetDb->pathForId(fitzel::AssetId::fromString(guid));
                if (!p.empty()) {
                    const std::string rel = relInside(proj, p);
                    it.action = Item::Action::Present;
                    it.target = rel.empty() ? p.filename().string() : rel;
                    it.why    = rel.empty() ? "part of the engine" : "the project already has it";
                    // ...perhaps under another name (an earlier import renamed
                    // it): what names it by name has to use that one.
                    const std::string was = fs::path(name).filename().string();
                    const std::string now = p.filename().string();
                    if (!rel.empty() && was != now) plan.renamedFiles.push_back({was, now});
                    taken.insert(it.target);
                    plan.items.push_back(it);
                    continue;
                }
            }
            if (ext == ".fmat") {
                json m;
                try { m = json::parse(bytes); } catch (const json::exception&) {}
                const json content = materialContent(m);
                const std::string mname = m.is_object() ? m.value("name", std::string()) : "";
                const auto match = std::find_if(mats.begin(), mats.end(), [&](const ProjectMaterial& pm) {
                    return !pm.guid.empty() && pm.name == mname && pm.content == content;
                });
                if (match != mats.end()) {
                    it.action = Item::Action::Present;
                    it.target = match->rel;
                    it.why    = "the project has this material already (\"" + mname + "\")";
                    if (!guid.empty() && guid != match->guid) plan.guidRemap.push_back({guid, match->guid});
                    taken.insert(it.target);
                    plan.items.push_back(it);
                    continue;
                }
            }
            if (!exists) {
                it.action = Item::Action::Add;
            } else if (readBytes(at, have) && have == bytes) {
                it.action = Item::Action::Present;
                it.why    = "the same file is already there";
                // Same bytes under another GUID: point at the one the project has.
                const std::string mine = diskGuid(at);
                if (!guid.empty() && !mine.empty() && guid != mine)
                    plan.guidRemap.push_back({guid, mine});
            } else {
                it.action = Item::Action::Rename;
                it.target = freeName(proj, name, taken);
                it.why    = "a different file called " + fs::path(name).filename().string() +
                            " is already there";
                plan.renamedFiles.push_back({fs::path(name).filename().string(),
                                             fs::path(it.target).filename().string()});
                renamedAny = true;
            }
            taken.insert(it.target);
            plan.items.push_back(it);
        }
    if (renamedAny && hasScripts)
        plan.notes.push_back("Scripts that name a renamed file by its old name will get the "
                             "project's own file of that name.");
    return true;
}

bool applyImport(const ImportTarget& dst, const Plan& plan, Applied& out, std::string& err) {
    out = {};
    if (dst.projectFolder.empty()) { err = "Open a project first."; return false; }
    ZipIn z;
    if (!z.load(plan.zipPath, err)) return false;
    const fs::path proj(dst.projectFolder);
    for (const Item& it : plan.items) {
        const fs::path at = proj / it.target;
        if (it.entry == plan.prefabEntry) out.prefabPath = at.generic_string();
        if (it.action == Item::Action::Present) continue;
        std::string bytes = z.read(it.entry);
        const std::string ext = extOf(it.entry);
        if (ext == ".fprefab" || ext == ".fmat") bytes = rewriteJson(plan, bytes);
        std::error_code ec;
        if (it.action == Item::Action::Update)
            fs::copy_file(at, fs::path(at.string() + ".bak"),
                          fs::copy_options::overwrite_existing, ec);
        if (!writeBytes(at, bytes)) { err = "Cannot write " + it.target; return false; }
        if (z.has(it.entry + ".meta") &&
            !writeBytes(fs::path(at.string() + ".meta"), z.read(it.entry + ".meta"))) {
            err = "Cannot write " + it.target + ".meta";
            return false;
        }
        out.written.push_back(it.target);
    }
    return true;
}

} // namespace prefabpkg