// The Procedural window's painted textures (ProcGraphPanel.hpp, "Painted
// textures"): the maps a Rock texture node made, into library materials, the
// GPU and the project's files.

#include "ProcGraphPanel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>

#include <imgui.h>
#include <stb_image_write.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/rhi/Device.hpp>

#include "Component.hpp"
#include "EditorContext.hpp"

namespace fs = std::filesystem;

namespace procui {

namespace {

std::string projectRoot(const fitzel::AssetDatabase& db) {
    for (const fitzel::AssetDatabase::Source& s : db.sources())
        if (s.kind == fitzel::AssetSourceKind::Project) return s.root.generic_string();
    return {};
}

std::string fileSafe(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return o.empty() ? std::string("rock") : o;
}

// The three files of a baked material, without their ending.
fs::path stemOf(const std::string& root, const MaterialDef& md) {
    return fs::path(root) / "textures" / "procedural" /
           (fileSafe(md.name) + "-" + md.assetId.toString().substr(0, 8));
}

// A material's name nobody in the library has yet.
std::string freeName(const std::vector<MaterialDef>& mats, const std::string& base) {
    auto taken = [&](const std::string& n) {
        return std::any_of(mats.begin(), mats.end(), [&](const MaterialDef& m) { return m.name == n; });
    };
    if (!taken(base)) return base;
    for (int k = 2;; ++k)
        if (!taken(base + " " + std::to_string(k))) return base + " " + std::to_string(k);
}

// The note beside a baked material's files: which bake they were painted
// from. Without it -- or with another bake in it than the scene's -- the files
// are older or newer than the scene (it was not saved after the last change,
// or they were copied in from elsewhere) and get painted again.
std::uint64_t readNote(const fs::path& stem) {
    std::FILE* f = std::fopen((stem.string() + ".bake").c_str(), "rb");
    if (!f) return 0;
    char buf[64] = {};
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return std::strtoull(buf, nullptr, 16);
}
bool writeNote(const std::string& stem, std::uint64_t hash) {
    std::FILE* f = std::fopen((stem + ".bake").c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "%016llx\n", static_cast<unsigned long long>(hash));
    return std::fclose(f) == 0;
}

bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) return fs::equivalent(a, b, ec);
    return a.lexically_normal() == b.lexically_normal();
}

// Put `px` (RGBA, top row first) on the GPU as `tex`. A texture the material
// already has for this map -- its own file's, or one made here before -- of
// the same size is written over in place, so every holder of it sees the new
// picture; otherwise a new one is made from memory. Either way the rows go up
// bottom first, the way the coordinates count them. A texture from memory is
// marked top-down, and the shader turns a top-down normal map's green over
// (see Texture::bottomUp), so a normal map going there is turned over first.
void upload(std::shared_ptr<fitzel::Texture>& tex, bool ours, const std::vector<std::uint8_t>& px, int w, int h,
            bool normal, std::vector<std::weak_ptr<fitzel::Texture>>& made) {
    std::vector<std::uint8_t> up(px.size());
    const std::size_t row = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h; ++y)
        std::copy_n(px.data() + static_cast<std::size_t>(h - 1 - y) * row, row, up.data() + static_cast<std::size_t>(y) * row);
    const bool inPlace = tex && ours && tex->isValid() && tex->width() == w && tex->height() == h;
    if (normal && !(inPlace && tex->bottomUp()))
        for (std::size_t i = 0; i < up.size(); i += 4) up[i + 1] = static_cast<std::uint8_t>(255 - up[i + 1]);
    if (inPlace && tex->update(up.data(), w, h, 4)) {
        fitzel::rhi::device().generateMipmaps(tex->id());
        return;
    }
    tex = std::make_shared<fitzel::Texture>(fitzel::Texture::fromPixels(up.data(), w, h, 4));
    made.erase(std::remove_if(made.begin(), made.end(), [](const std::weak_ptr<fitzel::Texture>& t) { return t.expired(); }),
               made.end());
    made.push_back(tex);
}

} // namespace

bool Panel::wornElsewhere(int id, const fitzel::AssetId& mat, std::uint64_t want) const {
    for (const Entity& o : m_d.ed.entities) {
        if (o.id == id) continue;
        // A graph that painted it: in the way only if it needs another bake.
        bool claimed = false;
        if (const ProcGraphComponent* pg = o.components.get<ProcGraphComponent>())
            for (const auto& [key, rec] : pg->baked)
                if (rec.material == mat) {
                    if (rec.hash != want) return true;
                    claimed = true;
                }
        if (claimed) continue;
        // Anything else wearing it shows whatever it holds now.
        if (const MeshComponent* mc = o.components.get<MeshComponent>())
            if (std::find(mc->mesh.faceMat.begin(), mc->mesh.faceMat.end(), mat) != mc->mesh.faceMat.end()) return true;
        if (const MaterialComponent* mt = o.components.get<MaterialComponent>(); mt && mt->material == mat) return true;
    }
    return false;
}

void Panel::resolveBakes(Entity& e, ProcGraphComponent& pg, const proc::Geo& geo, EditMesh& m) {
    std::vector<MaterialDef>& mats = m_d.ed.materials;
    fitzel::AssetDatabase& db = m_d.ed.assetDb;
    const std::string root = projectRoot(db);
    for (const proc::Bake& b : geo.bakes) {
        if (!b.maps) continue;
        ProcGraphComponent::Baked& rec = pg.baked[b.key];
        auto find = [&](const fitzel::AssetId& id) -> MaterialDef* {
            for (MaterialDef& md : mats)
                if (md.assetId == id) return &md;
            return nullptr;
        };
        MaterialDef* md = rec.material.valid() ? find(rec.material) : nullptr;
        // A material nobody repainted this session holds what its files do:
        // the bake its object saved with.
        auto shownFor = [&](const fitzel::AssetId& id) {
            const auto it = m_bakeShown.find(id);
            return it != m_bakeShown.end() ? it->second : rec.hash;
        };
        // To paint it over would change what someone else shows (a copy made
        // before the change, a prefab of an older state): theirs stays theirs,
        // and this object gets a material of its own.
        if (md && shownFor(md->assetId) != b.hash && wornElsewhere(e.id, md->assetId, b.hash)) md = nullptr;
        if (!md) {
            MaterialDef nm;
            nm.assetId = fitzel::AssetId::generate();
            nm.name    = freeName(mats, (e.name.empty() ? std::string("Rock") : e.name) + " " + b.key);
            mats.push_back(std::move(nm));
            md = &mats.back();
            rec.material = md->assetId;
            rec.hash     = 0;
        }
        if (shownFor(md->assetId) != b.hash || rec.hash != b.hash) {
            const proc::BakedMaps& mp = *b.maps;
            const fs::path stem = root.empty() ? fs::path() : stemOf(root, *md);
            // Ours to write over: made here, or the texture of its own file.
            auto own = [&](const std::shared_ptr<fitzel::Texture>& tex, const fitzel::AssetId& id, const char* sfx) {
                if (!tex) return false;
                for (const std::weak_ptr<fitzel::Texture>& t : m_bakeTex)
                    if (t.lock() == tex) return true;
                return id.valid() && !stem.empty() && samePath(db.pathForId(id), fs::path(stem.string() + sfx));
            };
            upload(md->tex, own(md->tex, md->texId, "_color.png"), mp.color, mp.w, mp.h, false, m_bakeTex);
            upload(md->normalTex, own(md->normalTex, md->normalTexId, "_normal.png"), mp.normal, mp.w, mp.h, true, m_bakeTex);
            upload(md->ormTex, own(md->ormTex, md->ormTexId, "_orm.png"), mp.orm, mp.w, mp.h, false, m_bakeTex);
            md->videoId = {};
            md->cameraName.clear();
            md->ormMean      = glm::vec2(mp.roughness, 1.0f);
            md->roughness    = mp.roughness;
            md->reflectivity = 0.0f;
            m_bakeShown[md->assetId] = b.hash;
            m_bakeWrite[md->assetId] = {b.maps, b.hash, ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0};
        }
        rec.hash = b.hash;
        for (fitzel::AssetId& f : m.faceMat)
            if (f == b.slot) f = md->assetId;
    }
}

void Panel::syncBakes() {
    const std::string root = projectRoot(m_d.ed.assetDb);
    for (Entity& e : m_d.ed.entities) {
        ProcGraphComponent* pg = e.components.get<ProcGraphComponent>();
        MeshComponent*      mc = e.components.get<MeshComponent>();
        if (!pg || !mc || pg->baked.empty()) continue;
        bool stale = false;
        for (const auto& [key, rec] : pg->baked) {
            auto it = m_bakeShown.find(rec.material);
            if (it == m_bakeShown.end()) {
                // First sight: what its files were painted from, by the note
                // written beside them. Without a project nothing can be read
                // -- or written -- and what is there stays.
                std::uint64_t has = rec.hash;
                if (!root.empty()) {
                    has = 0;
                    for (const MaterialDef& md : m_d.ed.materials)
                        if (md.assetId == rec.material) has = readNote(stemOf(root, md));
                }
                it = m_bakeShown.emplace(rec.material, has).first;
            }
            stale = stale || it->second != rec.hash;
        }
        if (!stale) continue;
        // Its own graph says what belongs in there. Only where it still makes
        // the bake the faces were cooked with: the faces stay as they are,
        // just the material under them is painted again (or, worn by someone
        // else now, replaced by one of its own).
        proc::Geo geo = proc::cookGeo(pg->graph, pg->graph.output);
        std::map<std::string, fitzel::AssetId> was;
        for (const auto& [key, rec] : pg->baked) was[key] = rec.material;
        geo.bakes.erase(std::remove_if(geo.bakes.begin(), geo.bakes.end(),
                                       [&](const proc::Bake& b) {
                                           const auto r = pg->baked.find(b.key);
                                           if (r == pg->baked.end() || r->second.hash == b.hash) return false;
                                           // A graph that cooks something else now (an older
                                           // painter's scene): left alone until it is edited.
                                           m_bakeShown[r->second.material] = r->second.hash;
                                           return true;
                                       }),
                        geo.bakes.end());
        EditMesh scratch;
        resolveBakes(e, *pg, geo, scratch);
        bool moved = false;
        for (const auto& [key, rec] : pg->baked) {
            const fitzel::AssetId old = was[key];
            if (!old.valid() || old == rec.material) continue;
            for (fitzel::AssetId& f : mc->mesh.faceMat)
                if (f == old) { f = rec.material; moved = true; }
        }
        if (moved) mc->touch();
    }
    flushBakes(false);
}

void Panel::flushBakes(bool now) {
    fitzel::AssetDatabase& db = m_d.ed.assetDb;
    auto findMat = [&](const fitzel::AssetId& id) -> MaterialDef* {
        for (MaterialDef& m : m_d.ed.materials)
            if (m.assetId == id) return &m;
        return nullptr;
    };
    // Files written: from now on the material's maps are the project's, by
    // their ids. The textures on the GPU stay -- they hold these pictures.
    for (auto it = m_bakeJobs.begin(); it != m_bakeJobs.end();) {
        if (!now && it->done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        const bool ok = it->done.get();
        if (MaterialDef* md = findMat(it->material); md && ok) {
            md->texId       = db.idForPath(it->stem + "_color.png");
            md->normalTexId = db.idForPath(it->stem + "_normal.png");
            md->ormTexId    = db.idForPath(it->stem + "_orm.png");
        } else if (!ok) {
            m_d.ed.status = "Rock texture: could not write " + it->stem + "_*.png";
        }
        it = m_bakeJobs.erase(it);
    }
    if (m_bakeWrite.empty()) return;
    const int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
    if (!now) {
        // A pause: no button held, nothing repainted for half a second.
        if (ImGui::GetCurrentContext() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
        for (const auto& [id, w] : m_bakeWrite)
            if (frame - w.frame < 30) return;
    }
    const std::string root = projectRoot(db);
    if (root.empty()) {
        if (m_bakeNote.empty()) {
            m_bakeNote = "Rock texture: save the project to keep the maps it painted.";
            m_d.ed.status = m_bakeNote;
        }
        return;
    }
    m_bakeNote.clear();
    for (auto it = m_bakeWrite.begin(); it != m_bakeWrite.end();) {
        MaterialDef* md = findMat(it->first);
        if (!md || !it->second.maps) { it = m_bakeWrite.erase(it); continue; }
        // One writer per set of files at a time; the next waits its turn.
        if (std::any_of(m_bakeJobs.begin(), m_bakeJobs.end(), [&](const BakeJob& j) { return j.material == it->first; })) {
            ++it;
            continue;
        }
        const std::string stem = stemOf(root, *md).string();
        const std::shared_ptr<const proc::BakedMaps> maps = it->second.maps;
        const std::uint64_t hash = it->second.hash;
        m_bakeJobs.push_back({it->first, stem, std::async(std::launch::async, [maps, stem, hash] {
                                  std::error_code ec;
                                  fs::create_directories(fs::path(stem).parent_path(), ec);
                                  bool ok = true;
                                  const std::pair<const char*, const std::vector<std::uint8_t>*> files[] = {
                                      {"_color.png", &maps->color}, {"_normal.png", &maps->normal}, {"_orm.png", &maps->orm}};
                                  for (const auto& [sfx, px] : files)
                                      ok = ok && stbi_write_png((stem + sfx).c_str(), maps->w, maps->h, 4, px->data(),
                                                                maps->w * 4) != 0;
                                  return ok && writeNote(stem, hash);
                              })});
        it = m_bakeWrite.erase(it);
    }
    if (now)
        for (BakeJob& j : m_bakeJobs) j.done.wait();
    if (now) flushBakes(true);
}

} // namespace procui
