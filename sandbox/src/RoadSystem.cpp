#include "RoadSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/asset/Vfs.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/world/Terrain.hpp>

#include "SandboxMath.hpp" // sampleSpline()

namespace {
// Roughly the distance between two centreline samples (sampleSpline aims for
// one every this many metres). Kept here because the corridor smoothing converts
// a metric sigma into a number of filter passes and needs the sample spacing to
// do it -- change it there and this must follow.
constexpr float kSampleStep = 2.0f;

// A road surface must be a colour/albedo map. We can't require "diff" in the
// name (the content folder's PNGs are all support maps and its albedos are .jpg,
// so that would hide every PNG); instead reject the known non-colour maps so
// custom jpg/png road textures still show up.
bool isRoadAlbedo(const std::string& filename) {
    const std::string ext = std::filesystem::path(filename).extension().string();
    if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") return false;
    for (const char* tok : {"_nor", "_disp", "_spec", "_rough", "_ao", "_arm",
                            "_metal", "_height", "_bump", "_mask", "_gl",
                            "translucent", "billboar", "tree"})
        if (filename.find(tok) != std::string::npos) return false;
    return true;
}

// Baseline clearance kept between the graded ground and the asphalt, on top of
// the measured sub-cell bulge (see baseBulge).
constexpr float kRoadClear = 0.02f;

// How far the carriageway may be rolled about its centreline. Well short of
// vertical, because the cross-fall enters the corridor grading as a tangent and
// that has to stay finite -- and because a road that wants to go past this is
// asking to be a loop, which a ribbon over an XZ centreline cannot express at
// all (its surface would need two heights over one ground position).
constexpr float kMaxBankDeg = 60.0f;

// How far the base terrain bulges above its own linear interpolation on a `cell`
// grid, at the four edge midpoints around `p`. This is exactly the part of the
// ground the edit field cannot flatten away: the terrain mesh samples the deltas
// bilinearly but evaluates the base noise exactly, at vertices finer than the
// grid. Never negative -- a dip below the interpolation is harmless.
float baseBulge(const fitzel::TerrainSettings& s, glm::vec2 p, float cell) {
    const float h0 = terrainBaseHeight(s, p.x, p.y);
    float bulge = 0.0f;
    const glm::vec2 dirs[4] = {{cell, 0.0f}, {-cell, 0.0f},
                               {0.0f, cell}, {0.0f, -cell}};
    for (const glm::vec2& d : dirs) {
        const float hn = terrainBaseHeight(s, p.x + d.x, p.y + d.y);
        const float hm = terrainBaseHeight(s, p.x + d.x * 0.5f, p.y + d.y * 0.5f);
        bulge = std::max(bulge, hm - 0.5f * (h0 + hn));
    }
    return bulge;
}

// Low-pass a longitudinal profile in place, ends held (anchored to the terrain).
void smooth(std::vector<float>& prof, int passes) {
    std::vector<float> tmp = prof;
    for (int p = 0; p < passes; ++p) {
        for (std::size_t i = 1; i + 1 < prof.size(); ++i)
            tmp[i] = 0.5f * prof[i] + 0.25f * (prof[i - 1] + prof[i + 1]);
        std::swap(prof, tmp);
    }
}

// The tokens texture packs use to mark a normal map. "_gl" alone isn't enough --
// the OpenGL-vs-DirectX suffix rides along on other maps too.
const char* const kNormalTokens[] = {"_nor", "_normal", "_nrm", "normalgl"};

// A normal map, and one we can actually decode: EXR normal maps sit next to the
// PNGs in the content pack, but the texture loader wants an LDR image here.
bool isRoadNormal(const std::string& filename) {
    const std::string ext = std::filesystem::path(filename).extension().string();
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga") return false;
    std::string low = filename;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* tok : kNormalTokens)
        if (low.find(tok) != std::string::npos) return true;
    return false;
}

// Squared distance from point p to segment [a,b], plus the projection param t.
float distToSeg(glm::vec2 p, glm::vec2 a, glm::vec2 b, float& t) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    t = (len2 > 1e-8f) ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 proj = a + ab * t;
    return glm::dot(p - proj, p - proj);
}
} // namespace

RoadSystem::RoadSystem(fitzel::Shader& lit, fitzel::AssetDatabase& assetDb,
                       fitzel::TerrainStreamer& streamer, const std::string& texDir)
    : m_assetDb(assetDb), m_streamer(streamer), m_texDir(texDir), m_mat(lit),
      m_bridgeMat(lit) {
    // uTint is written for the same reason as uRoadFade below: the shared lit
    // program keeps the last draw's tint otherwise.
    m_mat.set("uColorMode", 2).set("uTint", glm::vec3(1.0f));
    // Populate the picker from the built-in content textures (no project yet).
    refreshTextures(std::string());
    // Default surface. Loaded through the asset database (cached/deduplicated);
    // held as a shared handle so it stays alive while the material binds it.
    m_tex = m_assetDb.loadTexture(m_texDir + "/asphalt_02_diff_4k.jpg");
    if (m_tex) m_mat.setTexture("uTexture", *m_tex, 0);
    for (int i = 0; i < static_cast<int>(texFiles.size()); ++i)
        if (texFiles[i].find("asphalt") != std::string::npos) texSel = i;
    // ...and its grain, if the pack ships one. Asphalt is nearly flat shading
    // without it: a ribbon lit only by its geometry normal reads as painted-on.
    if (texSel >= 0 && texSel < static_cast<int>(texFiles.size())) {
        const std::string n = normalFor(texFiles[texSel]);
        if (!n.empty()) {
            setNormal(n);
            for (int i = 0; i < static_cast<int>(normFiles.size()); ++i)
                if (normFiles[i] == n) normSel = i;
        }
    }

    // No glow until the user asks for one, but the uniforms are written anyway so
    // the material never inherits another draw's emission (see the Renderer's
    // baseline note).
    applyEmission();

    // Bridges are cast concrete, and not user-selectable: a deck is structure, not
    // surface. uRoadFade is pinned off here on purpose -- the road turns the shared
    // lit program's edge fade on, and a material that never writes a uniform
    // inherits whatever the last one left in it.
    m_bridgeMat.set("uColorMode", 2).set("uRoadFade", 0.0f)
               .set("uTint", glm::vec3(1.0f));
    m_bridgeTex = m_assetDb.loadTexture(m_texDir + "/cracked_concrete_02_diff_4k.jpg");
    if (m_bridgeTex) m_bridgeMat.setTexture("uTexture", *m_bridgeTex, 0);

    // Same reasoning as applyEmission above: no puddle map yet, but both
    // materials say so themselves rather than inheriting it from a neighbour.
    applyWetness();
}

void RoadSystem::refreshTextures(const std::string& projectDir) {
    // Remember the current selections so a project switch doesn't reset them.
    const std::string prevSel =
        (texSel >= 0 && texSel < static_cast<int>(texFiles.size()))
            ? texFiles[texSel] : std::string();
    const std::string prevNorm =
        (normSel >= 0 && normSel < static_cast<int>(normFiles.size()))
            ? normFiles[normSel] : std::string();

    const std::string prevEmis =
        (emisSel >= 0 && emisSel < static_cast<int>(emisFiles.size()))
            ? emisFiles[emisSel] : std::string();

    texFiles.clear();
    m_texPaths.clear();
    normFiles.clear();
    m_normPaths.clear();
    emisFiles.clear();
    m_emisPaths.clear();
    std::unordered_set<std::string> seen;     // dedupe by display name
    std::unordered_set<std::string> seenNorm;
    std::unordered_set<std::string> seenEmis;

    auto add = [&](const std::filesystem::path& p) {
        const std::string name = p.filename().string();
        const std::string ext = p.extension().string();
        const bool decodable = (ext == ".png" || ext == ".jpg" ||
                                ext == ".jpeg" || ext == ".tga");
        // Normal maps: EVERY image the loader can decode, not just the ones whose
        // name carries a known token. Packs name them anything -- "..._n.png",
        // "bump.png", "surface-normalmap.png" -- and a picker that silently drops
        // what it doesn't recognise leaves the author staring at a list that is
        // missing the exact file they can see on disk, with nothing to say why.
        // isRoadNormal still earns its keep below: it decides what is ALSO a
        // surface or a glow map, and it drives the automatic partner lookup in
        // normalFor(), which is where guessing belongs.
        if (decodable && seenNorm.insert(name).second) {
            normFiles.push_back(name);
            m_normPaths.push_back(p.generic_string());
        }
        // A file marked as a normal map is not offered as a surface or a glow.
        if (isRoadNormal(name)) return;
        // Any other image can serve as a glow map -- a hand-painted stripe, a
        // "_mask", a whole neon pattern -- so this list is deliberately wider
        // than the surface list, which only wants tileable colour maps.
        if (decodable && seenEmis.insert(name).second) {
            emisFiles.push_back(name);
            m_emisPaths.push_back(p.generic_string());
        }
        if (!isRoadAlbedo(name) || !seen.insert(name).second) return;
        texFiles.push_back(name);
        m_texPaths.push_back(p.generic_string());
    };

    // Built-in content textures (flat scan, as before). Through the VFS, so an
    // exported game lists what the archive holds -- the scene stores a texture by
    // NAME, and a name nothing resolves to is a road without a surface.
    for (const std::string& f : fitzel::vfs::listFiles(m_texDir, false))
        add(std::filesystem::path(f));
    // Project-local textures (recursive), so surfaces dropped into the open
    // project show up too. Names already present in content are kept (not shadowed).
    if (!projectDir.empty())
        for (const std::string& f : fitzel::vfs::listFiles(projectDir, true))
            add(std::filesystem::path(f));

    // Sort display names and keep the parallel path lists aligned.
    auto sortPair = [](std::vector<std::string>& names, std::vector<std::string>& paths) {
        std::vector<int> order(names.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return names[a] < names[b]; });
        std::vector<std::string> sn, sp;
        sn.reserve(order.size()); sp.reserve(order.size());
        for (int i : order) { sn.push_back(names[i]); sp.push_back(paths[i]); }
        names.swap(sn);
        paths.swap(sp);
    };
    sortPair(texFiles, m_texPaths);
    sortPair(normFiles, m_normPaths);
    sortPair(emisFiles, m_emisPaths);

    // Restore the selections by name (else fall back: first surface, no normal).
    texSel = 0;
    for (int i = 0; i < static_cast<int>(texFiles.size()); ++i)
        if (texFiles[i] == prevSel) { texSel = i; break; }
    normSel = -1;
    for (int i = 0; i < static_cast<int>(normFiles.size()); ++i)
        if (normFiles[i] == prevNorm) { normSel = i; break; }
    emisSel = -1;
    for (int i = 0; i < static_cast<int>(emisFiles.size()); ++i)
        if (emisFiles[i] == prevEmis) { emisSel = i; break; }
    // A glow map that is no longer on disk (project switched away from it) has to
    // be dropped from the material too, or the road keeps glowing through a
    // texture the picker says isn't selected.
    if (emisSel < 0 && !prevEmis.empty()) setEmission(std::string());
}

std::string RoadSystem::normalFor(const std::string& file) const {
    // Swap the colour token for each normal token and see what the pack has:
    // asphalt_02_diff_4k.jpg -> asphalt_02_nor_gl_4k.png. Packs are consistent
    // about this, and it saves picking the obvious partner by hand every time.
    const std::string stem = std::filesystem::path(file).stem().string();
    for (const char* colour : {"_diff", "_diffuse", "_albedo", "_basecolor", "_col"}) {
        const std::size_t at = stem.find(colour);
        if (at == std::string::npos) continue;
        const std::string head = stem.substr(0, at);
        const std::string tail = stem.substr(at + std::strlen(colour));
        for (int i = 0; i < static_cast<int>(normFiles.size()); ++i) {
            const std::string ns = std::filesystem::path(normFiles[i]).stem().string();
            // Same pack (head) and same resolution/variant suffix (tail): the "4k"
            // in asphalt_02_nor_gl_4k has to match, or a 1k normal lands on a 4k
            // colour and the grain comes out the wrong size.
            if (ns.rfind(head, 0) == 0 && ns.size() >= tail.size() &&
                ns.compare(ns.size() - tail.size(), tail.size(), tail) == 0)
                return normFiles[i];
        }
    }
    return std::string();
}

void RoadSystem::setNormal(const std::string& file) {
    if (file.empty()) {
        m_normTex.reset();
        m_mat.set("uHasNormalMap", 0);
        normSel = -1;
        return;
    }
    std::string path;
    for (std::size_t i = 0; i < normFiles.size(); ++i)
        if (normFiles[i] == file) { path = m_normPaths[i]; break; }
    if (path.empty()) path = m_texDir + "/" + file;
    if (auto t = m_assetDb.loadTexture(path)) {
        m_normTex = t;
        m_mat.setTexture("uNormalMap", *m_normTex, 1); // unit 1: uTexture holds 0
        m_mat.set("uHasNormalMap", 1);
    }
}

void RoadSystem::setEmission(const std::string& file) {
    if (file.empty()) {
        m_emisTex.reset();
        emisSel = -1;
        applyEmission();
        return;
    }
    std::string path;
    for (std::size_t i = 0; i < emisFiles.size(); ++i)
        if (emisFiles[i] == file) { path = m_emisPaths[i]; break; }
    if (path.empty()) path = m_texDir + "/" + file;
    if (auto t = m_assetDb.loadTexture(path)) {
        m_emisTex = t;
        // Unit 3: 0 is the surface, 1 its normal map, 7 the shadow array -- the
        // same slot object materials use for their _Illum maps.
        m_mat.setTexture("uEmissionMap", *m_emisTex, 3);
    }
    applyEmission();
}

std::string RoadSystem::resolveTexPath(const std::string& name) const {
    const std::vector<std::string>* names[] = {&texFiles, &normFiles, &emisFiles};
    const std::vector<std::string>* paths[] = {&m_texPaths, &m_normPaths, &m_emisPaths};
    for (int l = 0; l < 3; ++l)
        for (std::size_t i = 0; i < names[l]->size() && i < paths[l]->size(); ++i)
            if ((*names[l])[i] == name) return (*paths[l])[i];
    return m_texDir + "/" + name;
}

void RoadSystem::setWetMap(const std::string& file) {
    wetMap = file;
    if (file.empty()) {
        m_wetTex.reset();
        applyWetness();
        return;
    }
    if (auto t = m_assetDb.loadTexture(resolveTexPath(file))) {
        m_wetTex = t;
        // Unit 4: 0 is the surface, 1 its normal, 2 the env probe, 3 the glow
        // map, 7 the shadow array. (3..6 are terrain units, and the terrain is a
        // different material -- units only have to be unique within one draw.)
        m_mat.setTexture("uWetMap", *m_wetTex, 4);
        m_bridgeMat.setTexture("uWetMap", *m_wetTex, 4);
    }
    applyWetness();
}

void RoadSystem::applyWetness() {
    const int   has = m_wetTex ? 1 : 0;
    const float var = std::clamp(wetVar, 0.0f, 1.0f);
    // The ribbon's UVs run 0..width/texTile across and metres/texTile along (see
    // loft), so one factor undoes the asphalt's tiling and re-tiles in metres.
    // `wetStretch` then lowers the frequency ALONG the drive only, which is the
    // shape real puddles have: water runs down the grade and the traffic drags it,
    // so a patch ends up longer than it is wide. 1 keeps them round.
    const float s   = (wetTile > 1e-4f) ? texTile / wetTile : 1.0f;
    const float str = std::max(wetStretch, 0.05f);
    const glm::vec2 scale(s, s / str);
    // Where the carriageway sits in the ribbon's u, so the shader can put the
    // gutter at the kerbs and the ruts under the wheels rather than at whatever u
    // happens to be. u runs from 0 at the outer edge of the left lip, so the
    // carriageway starts at edgeReach() metres and spans `width` -- both divided
    // by texTile, because that is what loft does to every column.
    const float lipU  = (texTile > 1e-4f) ? edgeReach() / texTile : 0.0f;
    const float spanU = (width > 1e-4f && texTile > 1e-4f) ? texTile / width : 0.0f;
    // uWetReflect is written HERE and nowhere else: the renderer's baseline
    // clears it for every other draw, so the carriageway is the only thing in the
    // scene that mirrors when it is wet.
    const float refl = std::clamp(wetReflect, 0.0f, 1.0f);
    m_mat.set("uHasWetMap", has).set("uWetVar", var)
         .set("uWetMapScale", scale).set("uWetReflect", refl)
         .set("uWetShore", std::max(wetShore, 0.01f))
         .set("uWetLat", glm::vec2(lipU, spanU));
    // The deck is a different mesh with its own UVs (see roadbridge::build), so it
    // gets the noise but not the road-shaped pooling: a gutter placed by the
    // ribbon's u would land somewhere arbitrary on it. y = 0 is that switch.
    m_bridgeMat.set("uHasWetMap", has).set("uWetVar", var)
               .set("uWetMapScale", scale).set("uWetReflect", refl)
               .set("uWetShore", std::max(wetShore, 0.01f))
               .set("uWetLat", glm::vec2(0.0f, 0.0f));
}

void RoadSystem::applyEmission() {
    m_mat.set("uEmission", emission)
         .set("uEmissionStrength", std::max(emissionStrength, 0.0f))
         .set("uHasEmissionMap", m_emisTex ? 1 : 0);
    // The ribbon's own UVs run 0..width/texTile across and metres/texTile along
    // (see loft). Undo both so the glow map spans the carriageway exactly once
    // across -- a stripe at the centre of the image lands on the centre line --
    // and repeats every `emissionTile` metres along the drive.
    const float uScale = (width > 1e-4f) ? texTile / width : 1.0f;
    const float vScale = (emissionTile > 1e-4f) ? texTile / emissionTile : 1.0f;
    m_mat.set("uEmissionUVScale", glm::vec2(uScale, vScale));
}

void RoadSystem::setSurface(const std::string& file) {
    // Resolve the display name to a full path via the scanned list; fall back to
    // the content dir for names not in the list (e.g. a scene saved with a
    // texture that is no longer present, or loaded before the list is built).
    std::string path;
    for (std::size_t i = 0; i < texFiles.size(); ++i)
        if (texFiles[i] == file) { path = m_texPaths[i]; break; }
    if (path.empty()) path = m_texDir + "/" + file;
    if (auto t = m_assetDb.loadTexture(path)) {
        m_tex = t;
        m_mat.setTexture("uTexture", *m_tex, 0);
    }
}

void RoadSystem::insertPoint(int at, glm::vec2 p, float lift, float bank) {
    at = std::clamp(at, 0, static_cast<int>(roadPts.size()));
    ptLift.resize(roadPts.size(), 0.0f); // heal a short array before indexing it
    ptBank.resize(roadPts.size(), 0.0f);
    roadPts.insert(roadPts.begin() + at, p);
    ptLift.insert(ptLift.begin() + at, lift);
    ptBank.insert(ptBank.begin() + at, bank);
    needsBuild = true;
}

void RoadSystem::erasePoint(int at) {
    if (at < 0 || at >= static_cast<int>(roadPts.size())) return;
    ptLift.resize(roadPts.size(), 0.0f);
    ptBank.resize(roadPts.size(), 0.0f);
    roadPts.erase(roadPts.begin() + at);
    ptLift.erase(ptLift.begin() + at);
    ptBank.erase(ptBank.begin() + at);
    needsBuild = true;
}

void RoadSystem::clearPoints() {
    roadPts.clear();
    ptLift.clear();
    ptBank.clear();
    needsBuild = true;
}

float RoadSystem::liftOf(int i) const {
    return (i >= 0 && i < static_cast<int>(ptLift.size())) ? ptLift[i] : 0.0f;
}

void RoadSystem::setLift(int i, float lift) {
    if (i < 0 || i >= static_cast<int>(roadPts.size())) return;
    ptLift.resize(roadPts.size(), 0.0f);
    ptLift[i] = lift;
    needsBuild = true;
}

float RoadSystem::bankOf(int i) const {
    return (i >= 0 && i < static_cast<int>(ptBank.size())) ? ptBank[i] : 0.0f;
}

void RoadSystem::setBank(int i, float deg) {
    if (i < 0 || i >= static_cast<int>(roadPts.size())) return;
    ptBank.resize(roadPts.size(), 0.0f);
    // Clamped well short of vertical: the cross-fall is applied as a tangent in
    // the corridor grading, and it has to stay finite. A road that wants to go
    // past this is asking to be a loop, which this model cannot express.
    ptBank[i] = std::clamp(deg, -kMaxBankDeg, kMaxBankDeg);
    needsBuild = true;
}

void RoadSystem::save(nlohmann::json& j) const {
    // Control points as a compact "x z x z ..." blob rather than an array of pairs:
    // a road is hundreds of points and this keeps the scene file readable.
    std::ostringstream rs;
    rs.precision(7);
    for (const glm::vec2& p : roadPts) rs << p.x << ' ' << p.y << ' ';
    // Heights ride in their own blob rather than as a third number per point, so
    // a scene written here still loads in a build that predates them.
    std::ostringstream ls;
    ls.precision(4);
    for (std::size_t i = 0; i < roadPts.size(); ++i) ls << liftOf(static_cast<int>(i)) << ' ';
    // Cross-fall rides in its own blob for the same reason the heights do: a
    // scene written here still loads in a build that predates banking.
    std::ostringstream bs;
    bs.precision(4);
    for (std::size_t i = 0; i < roadPts.size(); ++i) bs << bankOf(static_cast<int>(i)) << ' ';

    nlohmann::json bridges_ = nlohmann::json::array();
    for (const BridgeSpec& b : bridges) bridges_.push_back({b.a, b.b});
    nlohmann::json tunnels_ = nlohmann::json::array();
    for (const BridgeSpec& t : tunnels) tunnels_.push_back({t.a, t.b});

    nlohmann::json loops_ = nlohmann::json::array();
    for (const roadloop::Spec& l : loops)
        loops_.push_back({{"a", l.a}, {"b", l.b}, {"radius", l.radius}});

    nlohmann::json side_ = nlohmann::json::array();
    for (const roadside::Line& l : sideLines)
        side_.push_back({
            {"enabled",  l.enabled},
            {"kind",     static_cast<int>(l.kind)},
            {"model",    l.model},
            {"side",     l.side},
            {"onRoad",   l.onRoad},
            {"offset",   l.offset},
            {"spacing",  l.spacing},
            {"lift",     l.lift},
            {"sink",     l.sink},
            {"yaw",      l.yaw},
            {"scale",    l.scale},
            {"faceRoad", l.faceRoad},
            {"knockable",l.knockable},
            {"mass",     l.mass},
        });

    // Decals. One object per rule; the patches they produce are derived and never
    // written, exactly like the side objects above.
    nlohmann::json decals_ = nlohmann::json::array();
    for (const roaddecal::Decal& d : decals)
        decals_.push_back({
            {"enabled", d.enabled},
            {"texture", d.texture},
            {"dist",    d.dist},
            {"offset",  d.offset},
            {"length",  d.length},
            {"width",   d.width},
            {"repeat",  d.repeat},
            {"spacing", d.spacing},
            {"spin",    d.spin},
            {"blend",   static_cast<int>(d.blend)},
            {"opacity", d.opacity},
            {"cutoff",  d.cutoff},
            {"tint",    {d.tint.r, d.tint.g, d.tint.b}},
            {"glow",    {d.glow.r, d.glow.g, d.glow.b}},
            {"glowStrength", d.glowStrength},
            {"lift",    d.lift},
        });

    // City biomes. One object per biome; the buildings they produce are derived
    // and never written -- the whole point of CityGen (a four-hundred-tower
    // district costs these few hundred bytes in the scene file).
    nlohmann::json city_ = nlohmann::json::array();
    for (const city::Biome& b : biomes) {
        nlohmann::json mix = nlohmann::json::array();
        for (int i = 0; i < city::kStyleCount; ++i) mix.push_back(b.styleMix[i]);
        city_.push_back({
            {"enabled", b.enabled},   {"name", b.name},
            {"from", b.from},         {"to", b.to},          {"blend", b.blend},
            {"side", b.side},
            {"setback", b.setback},   {"setbackVar", b.setbackVar},
            {"frontage", b.frontage}, {"frontageVar", b.frontageVar},
            {"gap", b.gap},
            {"depth", b.depth},       {"depthVar", b.depthVar},
            {"fill", b.fill},
            {"blockLength", b.blockLength}, {"blockGap", b.blockGap},
            {"maxSlope", b.maxSlope}, {"maxDrop", b.maxDrop},
            {"floorsMin", b.floorsMin}, {"floorsMax", b.floorsMax},
            {"floorHeight", b.floorHeight},
            {"skylineScale", b.skylineScale}, {"skylineBias", b.skylineBias},
            {"twist", b.twist},
            {"styleMix", mix},
            {"skywayEvery", b.skywayEvery}, {"skywayHeight", b.skywayHeight},
            {"skywayWidth", b.skywayWidth}, {"signChance", b.signChance},
            {"palette", b.palette},
            {"glassTint", {b.glassTint.r, b.glassTint.g, b.glassTint.b}},
            {"frameTint", {b.frameTint.r, b.frameTint.g, b.frameTint.b}},
            {"accentColor", {b.accentColor.r, b.accentColor.g, b.accentColor.b}},
            {"baseTint", {b.baseTint.r, b.baseTint.g, b.baseTint.b}},
            {"accentStrength", b.accentStrength},
            {"neon", b.neon}, {"collider", b.collider}, {"seed", b.seed},
            {"weathering", b.weathering}, {"grime", b.grime},
            {"clutter", b.clutter}, {"clutterVar", b.clutterVar},
            {"deadNeon", b.deadNeon}, {"windowLit", b.windowLit},
            {"windowColor", {b.windowColor.r, b.windowColor.g, b.windowColor.b}},
        });
    }

    j = {
        {"name",      name},
        {"points",    rs.str()},
        {"lifts",     ls.str()},
        {"banks",     bs.str()},
        {"closed",    closed},
        {"enabled",   enabled},
        {"width",     width},
        {"edgeWidth", edgeWidth},
        {"edgeAngle", edgeAngle},
        {"texTile",   texTile},
        {"fadeWidth", fadeWidth},
        {"rainRings", rainRings},
        {"wetness",   wetness},
        {"wetMap",    wetMap},
        {"wetTile",   wetTile},
        {"wetVar",    wetVar},
        {"wetShore",  wetShore},
        {"wetStretch", wetStretch},
        {"wetReflect", wetReflect},
        {"grade",     grade},
        {"shoulder",  shoulder},
        // The surface goes by name, not index: the texture list is rebuilt from
        // disk each run, so an index would point somewhere else next time.
        {"surface",   (texSel >= 0 && texSel < static_cast<int>(texFiles.size()))
                          ? texFiles[texSel] : std::string()},
        {"normal",    (normSel >= 0 && normSel < static_cast<int>(normFiles.size()))
                          ? normFiles[normSel] : std::string()},
        {"glowMap",   (emisSel >= 0 && emisSel < static_cast<int>(emisFiles.size()))
                          ? emisFiles[emisSel] : std::string()},
        {"glow",      {emission.r, emission.g, emission.b}},
        {"glowStrength", emissionStrength},
        {"glowTile",     emissionTile},
        {"bridges",   bridges_},
        {"tunnels",   tunnels_},
        {"tunnelStyle", {
            {"clearHeight", tunnelStyle.clearHeight},
            {"sideClear",   tunnelStyle.sideClear},
            {"skirt",       tunnelStyle.skirt},
            {"portal",      tunnelStyle.portal},
            {"abutment",    tunnelStyle.abutment},
        }},
        {"loops",     loops_},
        {"bridgeStyle", {
            {"deckThick",   bridgeStyle.deckThick},
            {"overhang",    bridgeStyle.overhang},
            {"railHeight",  bridgeStyle.railHeight},
            {"railWidth",   bridgeStyle.railWidth},
            {"pierSpacing", bridgeStyle.pierSpacing},
            {"pierWidth",   bridgeStyle.pierWidth},
            {"abutment",    bridgeStyle.abutment},
        }},
        {"sideObjects", side_},
        {"decals",      decals_},
        {"cityEnabled", cityEnabled},
        {"cityBudget",  cityBudget},
        {"cityRange",   cityRange},
        {"cityMinPixels", cityMinPixels},
        {"biomes",      city_},
    };
}

void RoadSystem::load(const nlohmann::json& j) {
    // Every field defaults to what a fresh road has, so a scene saved before a
    // param existed loads as the road it was built as.
    //
    // The name is one of those: a scene from before roads were plural has none,
    // and comes back "" so RoadSet can number it by position instead.
    name = j.value("name", std::string());
    roadPts.clear();
    ptLift.clear();
    if (j.contains("points") && j["points"].is_string()) {
        std::istringstream rs(j["points"].get<std::string>());
        glm::vec2 p;
        while (rs >> p.x >> p.y) roadPts.push_back(p);
    }
    if (j.contains("lifts") && j["lifts"].is_string()) {
        std::istringstream ls(j["lifts"].get<std::string>());
        float h;
        while (ls >> h) ptLift.push_back(h);
    }
    ptBank.clear();
    if (j.contains("banks") && j["banks"].is_string()) {
        std::istringstream bs(j["banks"].get<std::string>());
        float b;
        while (bs >> b) ptBank.push_back(b);
    }
    // A scene from before heights or banking (or a truncated/overlong blob) is
    // filled out to match the points, so nothing downstream has to bounds-check
    // the triple.
    ptLift.resize(roadPts.size(), 0.0f);
    ptBank.resize(roadPts.size(), 0.0f);
    closed    = j.value("closed",    false);
    enabled   = j.value("enabled",   true);
    width     = j.value("width",     5.0f);
    // 0 for a road saved before raised edges existed -- it loads flat, as it was.
    edgeWidth = j.value("edgeWidth", 0.0f);
    edgeAngle = j.value("edgeAngle", 35.0f);
    texTile   = j.value("texTile",   8.0f);
    fadeWidth = j.value("fadeWidth", 0.0f);
    rainRings = j.value("rainRings", 1.0f);
    // 0 for a road saved before its own wetness existed: dry unless it rains.
    wetness   = j.value("wetness",   0.0f);
    // ...and an even sheen for one saved before the puddle map existed.
    wetTile   = j.value("wetTile",   26.0f);
    wetVar    = j.value("wetVar",     0.0f);
    // Roads saved before the puddles had shores load with the shape the first
    // version had: a soft edge and no stretch along the drive.
    wetShore   = j.value("wetShore",   0.18f);
    wetStretch = j.value("wetStretch", 1.0f);
    wetReflect = j.value("wetReflect", 0.45f);
    setWetMap(j.value("wetMap", std::string())); // loads it + applies the uniforms
    grade     = j.value("grade",     0.55f);
    shoulder  = j.value("shoulder",  3.0f);

    const std::string surf = j.value("surface", std::string());
    if (!surf.empty()) {
        setSurface(surf);
        for (int i = 0; i < static_cast<int>(texFiles.size()); ++i)
            if (texFiles[i] == surf) texSel = i;
    }

    // Normal map. Absent in scenes saved before it existed -- those get the one
    // that matches their surface, which is what they would have picked anyway.
    // An explicit "" means the user cleared it, and that is honoured.
    if (const auto n = j.find("normal"); n != j.end()) {
        const std::string nf = n->get<std::string>();
        setNormal(nf);
        for (int i = 0; i < static_cast<int>(normFiles.size()); ++i)
            if (normFiles[i] == nf) normSel = i;
    } else if (!surf.empty()) {
        const std::string nf = normalFor(surf);
        if (!nf.empty()) {
            setNormal(nf);
            for (int i = 0; i < static_cast<int>(normFiles.size()); ++i)
                if (normFiles[i] == nf) normSel = i;
        }
    }

    // Glow. Absent in scenes saved before it existed, and its default strength is
    // 0 -- so an older track loads exactly as dark as it was built.
    emissionStrength = j.value("glowStrength", 0.0f);
    emissionTile     = j.value("glowTile",     40.0f);
    emission         = glm::vec3(1.0f);
    if (const auto g = j.find("glow"); g != j.end() && g->is_array() && g->size() == 3)
        emission = glm::vec3((*g)[0].get<float>(), (*g)[1].get<float>(),
                             (*g)[2].get<float>());
    setEmission(j.value("glowMap", std::string())); // also (re)applies the uniforms
    for (int i = 0; i < static_cast<int>(emisFiles.size()); ++i)
        if (emisFiles[i] == j.value("glowMap", std::string())) emisSel = i;

    // Bridges are absent from scenes saved before they existed -- those roads
    // simply have none, which is exactly how they were built.
    tunnels.clear();
    if (j.contains("tunnels") && j["tunnels"].is_array())
        for (const auto& t : j["tunnels"])
            if (t.is_array() && t.size() == 2)
                tunnels.push_back({t[0].get<int>(), t[1].get<int>()});
    const roadtunnel::Params td;
    tunnelStyle = td;
    if (const auto st = j.find("tunnelStyle"); st != j.end()) {
        tunnelStyle.clearHeight = st->value("clearHeight", td.clearHeight);
        tunnelStyle.sideClear   = st->value("sideClear",   td.sideClear);
        tunnelStyle.skirt       = st->value("skirt",       td.skirt);
        tunnelStyle.portal      = st->value("portal",      td.portal);
        tunnelStyle.abutment    = st->value("abutment",    td.abutment);
    }

    bridges.clear();
    if (j.contains("bridges") && j["bridges"].is_array())
        for (const auto& b : j["bridges"])
            if (b.is_array() && b.size() == 2)
                bridges.push_back({b[0].get<int>(), b[1].get<int>()});

    // Loops. Absent in scenes saved before they existed -> none, which is how
    // those roads drove.
    loops.clear();
    if (const auto lj = j.find("loops"); lj != j.end() && lj->is_array())
        for (const auto& e : *lj) {
            roadloop::Spec sp;
            sp.a      = e.value("a", 0);
            sp.b      = e.value("b", 0);
            sp.radius = e.value("radius", 12.0f);
            loops.push_back(sp);
        }

    const roadbridge::Params bd;
    bridgeStyle = bd;
    if (const auto st = j.find("bridgeStyle"); st != j.end()) {
        bridgeStyle.deckThick   = st->value("deckThick",   bd.deckThick);
        bridgeStyle.overhang    = st->value("overhang",    bd.overhang);
        bridgeStyle.railHeight  = st->value("railHeight",  bd.railHeight);
        bridgeStyle.railWidth   = st->value("railWidth",   bd.railWidth);
        bridgeStyle.pierSpacing = st->value("pierSpacing", bd.pierSpacing);
        bridgeStyle.pierWidth   = st->value("pierWidth",   bd.pierWidth);
        bridgeStyle.abutment    = st->value("abutment",    bd.abutment);
    }

    // Side objects. Absent in scenes saved before they existed -> none, which is
    // how those roads looked. Each field defaults to its Kind's preset, so a blob
    // written by an older/leaner build still loads as a sensible line.
    sideLines.clear();
    if (const auto so = j.find("sideObjects"); so != j.end() && so->is_array())
        for (const auto& e : *so) {
            roadside::Line l =
                roadside::preset(static_cast<roadside::Kind>(e.value("kind", 0)));
            l.enabled  = e.value("enabled",  l.enabled);
            l.model    = e.value("model",    std::string());
            l.side     = e.value("side",     l.side);
            l.onRoad   = e.value("onRoad",   l.onRoad);
            l.offset   = e.value("offset",   l.offset);
            l.spacing  = e.value("spacing",  l.spacing);
            l.lift     = e.value("lift",     l.lift);
            l.sink     = e.value("sink",     l.sink);
            l.yaw       = e.value("yaw",       l.yaw);
            l.scale     = e.value("scale",     l.scale);
            l.faceRoad  = e.value("faceRoad",  l.faceRoad);
            l.knockable = e.value("knockable", l.knockable);
            l.mass      = e.value("mass",      l.mass);
            sideLines.push_back(std::move(l));
        }

    // Decals. Absent in scenes saved before they existed -> none, which is how
    // those roads looked. Every field falls back to a default Decal's.
    decals.clear();
    if (const auto dc = j.find("decals"); dc != j.end() && dc->is_array())
        for (const auto& e : *dc) {
            roaddecal::Decal d;
            d.enabled = e.value("enabled", d.enabled);
            d.texture = e.value("texture", std::string());
            d.dist    = e.value("dist",    d.dist);
            d.offset  = e.value("offset",  d.offset);
            d.length  = e.value("length",  d.length);
            d.width   = e.value("width",   d.width);
            d.repeat  = e.value("repeat",  d.repeat);
            d.spacing = e.value("spacing", d.spacing);
            d.spin    = e.value("spin",    d.spin);
            d.blend   = static_cast<roaddecal::Blend>(
                e.value("blend", static_cast<int>(d.blend)));
            d.opacity = e.value("opacity", d.opacity);
            d.cutoff  = e.value("cutoff",  d.cutoff);
            if (const auto t = e.find("tint"); t != e.end() && t->is_array() && t->size() == 3)
                d.tint = glm::vec3((*t)[0].get<float>(), (*t)[1].get<float>(),
                                   (*t)[2].get<float>());
            if (const auto g = e.find("glow"); g != e.end() && g->is_array() && g->size() == 3)
                d.glow = glm::vec3((*g)[0].get<float>(), (*g)[1].get<float>(),
                                   (*g)[2].get<float>());
            d.glowStrength = e.value("glowStrength", d.glowStrength);
            d.lift         = e.value("lift",         d.lift);
            decals.push_back(std::move(d));
        }

    // City biomes. Absent in scenes saved before they existed -> no city, which
    // is how those tracks looked. Every field falls back to a default Biome's, so
    // a blob from a leaner build still loads as something buildable.
    cityEnabled = j.value("cityEnabled", true);
    cityBudget  = j.value("cityBudget",  400);
    cityRange   = j.value("cityRange",   1200.0f);
    // A road saved before the screen-size cull gets it anyway (the struct's own
    // default, not 0): it exists because honouring cityRange to the metre made
    // every old scene pay for buildings too small to make out, and leaving those
    // scenes on the slow path would be shipping the bug with an opt-out.
    cityMinPixels = j.value("cityMinPixels", 7.0f); // keep in step with the header
    biomes.clear();
    auto vec3Of = [](const nlohmann::json& o, const char* key, glm::vec3 def) {
        const auto it = o.find(key);
        if (it == o.end() || !it->is_array() || it->size() != 3) return def;
        return glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(),
                         (*it)[2].get<float>());
    };
    if (const auto cb = j.find("biomes"); cb != j.end() && cb->is_array())
        for (const auto& e : *cb) {
            city::Biome b;
            b.enabled     = e.value("enabled", b.enabled);
            b.name        = e.value("name", b.name);
            b.from        = e.value("from", b.from);
            b.to          = e.value("to", b.to);
            b.blend       = e.value("blend", b.blend);
            b.side        = e.value("side", b.side);
            b.setback     = e.value("setback", b.setback);
            b.setbackVar  = e.value("setbackVar", b.setbackVar);
            b.frontage    = e.value("frontage", b.frontage);
            b.frontageVar = e.value("frontageVar", b.frontageVar);
            b.gap         = e.value("gap", b.gap);
            b.depth       = e.value("depth", b.depth);
            b.depthVar    = e.value("depthVar", b.depthVar);
            b.fill        = e.value("fill", b.fill);
            b.blockLength = e.value("blockLength", b.blockLength);
            b.blockGap    = e.value("blockGap", b.blockGap);
            b.maxSlope    = e.value("maxSlope", b.maxSlope);
            b.maxDrop     = e.value("maxDrop", b.maxDrop);
            b.floorsMin   = e.value("floorsMin", b.floorsMin);
            b.floorsMax   = e.value("floorsMax", b.floorsMax);
            b.floorHeight = e.value("floorHeight", b.floorHeight);
            b.skylineScale = e.value("skylineScale", b.skylineScale);
            b.skylineBias  = e.value("skylineBias", b.skylineBias);
            b.twist        = e.value("twist", b.twist);
            if (const auto m = e.find("styleMix"); m != e.end() && m->is_array())
                for (int i = 0; i < city::kStyleCount &&
                                i < static_cast<int>(m->size()); ++i)
                    b.styleMix[i] = (*m)[i].get<float>();
            b.skywayEvery  = e.value("skywayEvery", b.skywayEvery);
            b.skywayHeight = e.value("skywayHeight", b.skywayHeight);
            b.skywayWidth  = e.value("skywayWidth", b.skywayWidth);
            b.signChance   = e.value("signChance", b.signChance);
            b.palette      = e.value("palette", b.palette);
            b.glassTint    = vec3Of(e, "glassTint",   b.glassTint);
            b.frameTint    = vec3Of(e, "frameTint",   b.frameTint);
            b.accentColor  = vec3Of(e, "accentColor", b.accentColor);
            b.baseTint     = vec3Of(e, "baseTint",    b.baseTint);
            b.accentStrength = e.value("accentStrength", b.accentStrength);
            b.neon         = e.value("neon", b.neon);
            b.collider     = e.value("collider", b.collider);
            b.seed         = e.value("seed", b.seed);
            b.weathering   = e.value("weathering", b.weathering);
            b.grime        = e.value("grime", b.grime);
            b.clutter      = e.value("clutter", b.clutter);
            b.clutterVar   = e.value("clutterVar", b.clutterVar);
            b.deadNeon     = e.value("deadNeon", b.deadNeon);
            b.windowLit    = e.value("windowLit", b.windowLit);
            if (e.contains("windowColor") && e["windowColor"].is_array() &&
                e["windowColor"].size() == 3)
                b.windowColor = glm::vec3(e["windowColor"][0].get<float>(),
                                          e["windowColor"][1].get<float>(),
                                          e["windowColor"][2].get<float>());
            biomes.push_back(std::move(b));
        }
}

std::vector<glm::vec2> RoadSystem::sampleCenterlineXZ(
        std::vector<int>* ptSample) const {
    // The curve itself lives in SandboxMath (sampleSpline), shared with the
    // fence/wall/track paths -- one definition of how a spline bends, so a wall
    // laid alongside a road cannot drift away from it through a corner.
    return sampleSpline(roadPts, closed, ptSample);
}

void RoadSystem::loft(const std::vector<glm::vec2>& center,
                      const std::vector<float>& height,
                      const std::vector<float>& bank,
                      const std::vector<roadloop::Loop>& loops) {
    fitzel::MeshData md;
    m_centerline = center; // flat centre for vegetation masking
    m_centerlineY = height; // road surface height per sample (deck top over bridges)
    m_centerlineBank = bank;
    m_centerlineBank.resize(center.size(), 0.0f);

    // Along-road UV wrap. `v` is metres/texTile accumulated from the start line,
    // so on a few-kilometre circuit it climbs into the hundreds -- and a float
    // has no bits to spare there. Two things break at that magnitude, both of
    // them subtle and both of them view-dependent: the sampler quantises
    // sub-texel positions, and dFdx(vUV) -- which selects the mip level and
    // builds the normal-map basis -- loses most of its precision to
    // cancellation, so the chosen mip flickers between quads. The result is a
    // fine banding across the carriageway that shifts as you turn: moire.
    //
    // Restarting `v` periodically keeps the numbers small. The restart is only
    // invisible if it happens after a WHOLE number of repeats -- of the surface
    // texture *and* of the emission map, which tiles independently every
    // `emissionTile` metres (see applyEmission). So the wrap distance is picked
    // in metres as a common multiple of both. If the two tilings have no small
    // common multiple (arbitrary user values), the wrap is pushed out to a
    // distance no real track reaches, i.e. it simply doesn't happen -- a track
    // that long would show a seam, which is worse than the banding.
    const float wrapDist = [&] {
        float period = texTile;                    // always a whole surface repeat
        if (emissionTile > 1e-4f) {
            bool found = false;
            for (int k = 1; k <= 64 && !found; ++k) {
                const float m = static_cast<float>(k) * texTile / emissionTile;
                if (std::fabs(m - std::round(m)) < 1e-3f) {
                    period = static_cast<float>(k) * texTile;
                    found  = true;
                }
            }
            if (!found) return 1.0e6f; // incommensurable tilings: never wrap
        }
        if (period < 1e-4f) return 1.0e6f;
        // Round up to ~256 m of road per wrap: small enough to keep v in the low
        // tens, large enough that the duplicated rungs stay negligible.
        return period * std::max(1.0f, std::floor(256.0f / period));
    }();

    const float half = width * 0.5f;

    // --- The cross-section ---------------------------------------------------
    // A list of columns across the road, each an offset from the centreline, a
    // rise above it and the surface normal there -- all in the SECTION's own 2D
    // frame, so the bank rolls the whole profile by rotating two vectors rather
    // than by special-casing each piece.
    //
    // Plain road: two columns, the left and right edges. With raised edges: six,
    // because the lip needs its own normal. The carriageway edge appears twice at
    // the same point, once carrying the flat road's normal and once the lip's --
    // that duplicate is what puts a crease where the wall starts instead of a
    // smeared fillet, and it costs two vertices per rung.
    struct Col { float off, rise; glm::vec2 n; float u; };
    std::vector<Col> section;
    {
        const float reach = edgeReach(), rise = edgeRise();
        const glm::vec2 flat(0.0f, 1.0f);
        if (reach > 1e-4f || rise > 1e-4f) {
            // Outward-and-up on the right is (cos a, sin a); its normal leans back
            // over the road, which is what makes the lip catch a craft rather than
            // launch it. Mirrored on the left.
            const glm::vec2 nR = glm::normalize(glm::vec2(-rise, reach));
            const glm::vec2 nL = glm::vec2(-nR.x, nR.y);
            section = {
                {-half - reach, rise, nL, 0.0f},
                {-half,         0.0f, nL, reach},
                {-half,         0.0f, flat, reach},
                { half,         0.0f, flat, reach + width},
                { half,         0.0f, nR, reach + width},
                { half + reach, rise, nR, reach + width + reach},
            };
        } else {
            section = {{-half, 0.0f, flat, 0.0f}, {half, 0.0f, flat, width}};
        }
        for (Col& c : section) c.u /= texTile;   // metres across -> texture u
    }
    const int cols = static_cast<int>(section.size());

    float vlen = 0.0f;
    float mOff = 0.0f;          // metres already wrapped away
    std::vector<bool> link;     // link[k]: is there a quad between rung k and k+1?

    // Stations a loop stands in place of. The turn advances exactly the distance
    // between its two control points while it goes round, so the ground it covers
    // is not road the loop happens to fly over -- it is the same stretch of road,
    // stood on end. Leaving the ribbon in as well drew a flat carriageway running
    // straight through the middle of the turn, which is what made a loop read as a
    // hoop parked beside the road. The centreline itself is untouched: the grading,
    // the lap timing, the roadside instancing and the rivals all still follow it.
    auto standsOnEnd = [&](std::size_t i) {
        const int k = static_cast<int>(i);
        for (const roadloop::Loop& lp : loops)
            if (lp.sa >= 0 && k >= lp.sa && k < lp.sb) return true;
        return false;
    };

    // One rung = the whole section placed at this station. The normals are no
    // longer a constant up: a banked section leans, and a ribbon that leans while
    // claiming to face straight up is lit as if it were flat -- the one thing that
    // would give the whole feature away at a glance.
    auto pushRung = [&](const glm::vec3& C, const glm::vec3& sideDir,
                        const glm::vec3& up, float v, bool linkNext) {
        for (const Col& c : section)
            md.vertices.push_back({C + sideDir * c.off + up * c.rise,
                                   sideDir * c.n.x + up * c.n.y,
                                   {c.u, v}});
        link.push_back(linkNext);
    };

    for (std::size_t i = 0; i < center.size(); ++i) {
        glm::vec2 fwd = (i == 0)                 ? center[1] - center[0]
                      : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                                 : center[i + 1] - center[i - 1];
        if (glm::length(fwd) < 1e-4f) fwd = glm::vec2(0, 1);
        fwd = glm::normalize(fwd);
        // Perpendicular in XZ, matching cross((0,1,0), fwd) so the ribbon winds
        // front-face-up (see the index order below).
        const glm::vec2 side(fwd.y, -fwd.x);
        if (i > 0) vlen += glm::length(center[i] - center[i - 1]);

        // Roll the section about the centreline. Positive bank drops the RIGHT
        // edge, so a right-hand corner banks into the turn. The centreline keeps
        // its profile height -- the two edges move oppositely around it, which is
        // what makes banking a tilt rather than a lift.
        const float br = glm::radians((i < bank.size()) ? bank[i] : 0.0f);
        const float cb = std::cos(br), sb = std::sin(br);
        const glm::vec3 sideDir(side.x * cb, -sb, side.y * cb);  // centre -> right
        const glm::vec3 C(center[i].x, height[i], center[i].y);
        // Perpendicular to sideDir in the plane it shares with world up, i.e. the
        // section's "up", rolled by the same angle.
        const glm::vec3 nrm(side.x * sb, cb, side.y * sb);

        const bool linkNext = (i + 1 < center.size()) && !standsOnEnd(i);
        float v = (vlen - mOff) / texTile;
        if (vlen - mOff >= wrapDist && i + 1 < center.size()) {
            pushRung(C, sideDir, nrm, v, /*linkNext=*/false); // closes the running strip
            mOff += wrapDist;
            v     = (vlen - mOff) / texTile;
            pushRung(C, sideDir, nrm, v, linkNext);           // ...and restarts it here
        } else {
            pushRung(C, sideDir, nrm, v, linkNext);
        }
    }
    // Two triangles per linked rung pair per column pair, wound CCW-from-above
    // (front faces up). The duplicated carriageway edges are skipped: their two
    // columns sit at the same point, so the quad between them has no area.
    for (std::size_t k = 0; k + 1 < link.size(); ++k) {
        if (!link[k]) continue;
        for (int c = 0; c + 1 < cols; ++c) {
            if (section[c].off == section[c + 1].off &&
                section[c].rise == section[c + 1].rise)
                continue;                       // the crease seam, not a surface
            const auto l0 = static_cast<std::uint32_t>(k * cols + c);
            const auto l1 = static_cast<std::uint32_t>((k + 1) * cols + c);
            md.indices.insert(md.indices.end(),
                              {l0, l1, l0 + 1, l0 + 1, l1, l1 + 1});
        }
    }
    m_mesh  = fitzel::Mesh::create(md);
    m_verts = static_cast<int>(md.vertices.size());

    // Keep the CPU geometry for the physics mesh collider (Play mode).
    m_collVerts.clear();
    m_collVerts.reserve(md.vertices.size());
    for (const fitzel::Vertex& vtx : md.vertices) m_collVerts.push_back(vtx.position);
    m_collIndices = md.indices;
}

void RoadSystem::buildLoops(const Layout& lo) {
    m_loops = lo.loops;
    fitzel::MeshData md;
    roadloop::build(m_loops, width, texTile, md);
    m_loopVerts = static_cast<int>(md.vertices.size());
    m_loopMesh  = fitzel::Mesh::create(md);
    if (md.vertices.empty()) return;
    // Merge into the road's collider, exactly as the bridge decks do -- a loop is
    // something you crash into as much as something you drive on.
    const auto base = static_cast<std::uint32_t>(m_collVerts.size());
    m_collVerts.reserve(m_collVerts.size() + md.vertices.size());
    for (const fitzel::Vertex& v : md.vertices) m_collVerts.push_back(v.position);
    m_collIndices.reserve(m_collIndices.size() + md.indices.size());
    for (std::uint32_t i : md.indices) m_collIndices.push_back(base + i);
}

void RoadSystem::buildConcrete(const Layout& lo) {
    fitzel::MeshData md;
    // The deck carries the whole section: with raised edges the carriageway is
    // wider than `width`, and a deck built to the bare width would leave the lip
    // hanging over thin air.
    roadbridge::build(lo.center, lo.prof, lo.ground, lo.bank, lo.spans,
                      surfaceHalf() * 2.0f, bridgeStyle, md);
    // The bores go into the same mesh: same concrete, same material, same collider
    // merge below. Also built to the full section width, for the mirrored reason a
    // deck is -- walls set to the bare width would stand on the raised edges.
    roadtunnel::build(lo.center, lo.prof, lo.bank, lo.bores,
                      surfaceHalf() * 2.0f, tunnelStyle, md);
    m_bridgeVerts = static_cast<int>(md.vertices.size());
    if (md.indices.empty()) { m_bridgeMesh = fitzel::Mesh(); return; }
    m_bridgeMesh = fitzel::Mesh::create(md);

    // The concrete collides as part of the one static road mesh: a deck because it
    // is the only ground there is up here, its parapets because they are what keep
    // a car from driving off the side, and a bore's walls because a tunnel you can
    // drive out through the side of is a corridor with scenery in it.
    const auto base = static_cast<std::uint32_t>(m_collVerts.size());
    for (const fitzel::Vertex& vtx : md.vertices) m_collVerts.push_back(vtx.position);
    m_collIndices.reserve(m_collIndices.size() + md.indices.size());
    for (std::uint32_t idx : md.indices) m_collIndices.push_back(base + idx);
}

std::vector<float> RoadSystem::pointRamp(const std::vector<float>& perPoint,
                                         const std::vector<int>& ptSample,
                                         std::size_t samples) const {
    if (samples == 0 || std::none_of(perPoint.begin(), perPoint.end(),
                                     [](float v) { return v != 0.0f; }))
        return {}; // nothing set: the caller skips the whole addition
    const int n = static_cast<int>(roadPts.size());
    auto at = [&](int i) {
        return (i >= 0 && i < static_cast<int>(perPoint.size())) ? perPoint[i] : 0.0f;
    };
    std::vector<float> off(samples, 0.0f);
    // Linear between control points -- predictable, and unlike a spline it cannot
    // overshoot and dip a raised stretch back into the ground it is meant to
    // clear (or roll a bank past the angle the user asked for).
    for (std::size_t k = 0; k + 1 < ptSample.size(); ++k) {
        const int a = ptSample[k], b = ptSample[k + 1];
        if (b <= a) continue;
        const float la = at(static_cast<int>(k));
        // The closing entry wraps to point 0 on a loop and repeats the last point
        // on an open road; the modulo covers both.
        const float lb = at(n > 0 ? static_cast<int>(k + 1) % n : 0);
        for (int i = a; i <= b && i < static_cast<int>(off.size()); ++i)
            off[i] = glm::mix(la, lb, static_cast<float>(i - a) / (b - a));
    }
    // Round off the kink each control point leaves in the ramp. Ends are held, so
    // a road that starts or ends lifted (or banked) keeps that value exactly.
    smooth(off, 5);
    return off;
}

RoadSystem::Layout RoadSystem::layout() const {
    Layout lo;
    std::vector<int> ptSample;
    lo.center = sampleCenterlineXZ(&ptSample);
    if (lo.center.size() < 2) return lo;

    const fitzel::TerrainSettings& s = m_streamer.settings();

    // Smoothing below counts filter passes, but samples are ~kSampleStep metres
    // apart, so express the passes as the world-space distance they should blur
    // over: a [1/4,1/2,1/4] pass has variance spacing^2/2, so P passes reach
    // sigma = spacing*sqrt(P/2). Keeps the grade slider metric rather than tied
    // to the sample density.
    auto passesFor = [](float sigmaMetres) {
        const float p = 2.0f * (sigmaMetres / kSampleStep) *
                                (sigmaMetres / kSampleStep);
        return std::clamp(static_cast<int>(std::lround(p)), 1, 600);
    };

    // Longitudinal profile: start from the *base* (procedural) terrain height under
    // each sample, then low-pass it so the road grades smoothly instead of
    // following every bump. Anchor the ends to the ground so the road meets the
    // terrain where it begins/ends. Working from the base makes rebuilds idempotent
    // (independent of any corridor already baked in), and gives the piers ground to
    // stand on that doesn't move when the road is rebuilt.
    lo.ground.resize(lo.center.size());
    for (std::size_t i = 0; i < lo.center.size(); ++i)
        lo.ground[i] = terrainBaseHeight(s, lo.center[i].x, lo.center[i].y);

    lo.prof = lo.ground;
    smooth(lo.prof, passesFor(3.0f + grade * 15.0f));

    // Per-point height offsets, added *after* the smoothing -- smoothing them
    // together with the ground would flatten the very edit the user just made.
    const std::vector<float> off = pointRamp(ptLift, ptSample, lo.center.size());
    if (!off.empty())
        for (std::size_t i = 0; i < lo.prof.size(); ++i) lo.prof[i] += off[i];

    // Cross-fall, ramped the same way. Kept out of `prof` deliberately: the
    // profile is the height of the CENTRELINE, and banking tilts the section
    // about it -- folding the two together would raise the whole road instead of
    // rolling it.
    lo.bank = pointRamp(ptBank, ptSample, lo.center.size());
    if (lo.bank.empty()) lo.bank.assign(lo.center.size(), 0.0f);

    // Each bridge the user asked for, as a run of samples, looked up through the
    // control point -> sample map. Specs naming points that have since been
    // deleted are skipped rather than clamped: silently bridging somewhere else
    // is worse than nothing. A bridge always runs the low-to-high way round, so
    // on a closed loop one drawn across the seam takes the long way instead of
    // the short one.
    std::vector<roadbridge::Span> cores;
    const int pts  = std::min(static_cast<int>(roadPts.size()),
                              static_cast<int>(ptSample.size()));
    const int last = static_cast<int>(lo.center.size()) - 1;
    for (const BridgeSpec& spec : bridges) {
        const int p0 = std::min(spec.a, spec.b), p1 = std::max(spec.a, spec.b);
        if (p0 < 0 || p1 >= pts || p0 == p1) continue;
        const int sa = ptSample[p0], sb = ptSample[p1];
        if (sa >= last || sb > last) continue;
        cores.push_back({sa, sb});
    }

    // Fly the road along each bridge and work out where the terrain must stop being
    // pulled up to it.
    lo.spans = roadbridge::plan(lo.center, lo.prof, cores, bridgeStyle, lo.gradeW);

    // Then bore the tunnels, by exactly the same bookkeeping.
    std::vector<roadtunnel::Span> tcores;
    for (const BridgeSpec& spec : tunnels) {
        const int p0 = std::min(spec.a, spec.b), p1 = std::max(spec.a, spec.b);
        if (p0 < 0 || p1 >= pts || p0 == p1) continue;
        const int sa = ptSample[p0], sb = ptSample[p1];
        if (sa >= last || sb > last) continue;
        tcores.push_back({sa, sb});
    }
    // AFTER the bridges, and that order decides a stretch asked to be both: the
    // deck lifts the profile with max(), the bore then holds it down with min(),
    // so the tunnel wins. Which is the right way round -- a road cannot fly over
    // a hill it is also driving through, and the hill is the thing that is there.
    std::vector<float> boreW;
    lo.bores = roadtunnel::plan(lo.center, lo.prof, tcores, tunnelStyle, boreW);
    // Both weights ASSIGN, so they have to be combined here rather than chained:
    // the terrain must stop being graded wherever EITHER structure says so, which
    // is the elementwise minimum.
    for (std::size_t i = 0; i < lo.gradeW.size() && i < boreW.size(); ++i)
        lo.gradeW[i] = std::min(lo.gradeW[i], boreW[i]);
    // Loops ride on the profile the bridges have already settled, so a loop on a
    // bridged stretch leaves from the deck rather than from the gorge below it.
    lo.loops = roadloop::plan(lo.center, lo.prof, ptSample, loops, width);
    // The chords meet the road at an angle; round those two kinks off so a bridge
    // entrance isn't a bump. A straight chord is a fixed point of this filter, so
    // only the tangents move.
    if (!lo.spans.empty() || !lo.bores.empty()) smooth(lo.prof, passesFor(5.0f));
    return lo;
}

bool RoadSystem::surfaceHeightAt(const glm::vec2& xz, float halfWidth, float& outY,
                                 float maxY) const {
    const std::size_t n = m_centerline.size();
    if (n < 2 || m_centerlineY.size() != n) return false;
    const std::size_t segs = closed ? n : n - 1;
    const float lim = halfWidth * halfWidth;
    // NEAREST among the surfaces at or below the ceiling. Both halves of that are
    // load-bearing.
    //
    // Nearest, because anything that picked by HEIGHT would make the answer
    // depend on the caller's own height -- and the caller is a hover craft whose
    // height depends on the answer. That loop bounces: a road sample two metres
    // uphill slips in and out of the ceiling as the craft bobs, the ground under
    // it moves by the slope, and the hover spring chases it. (It did; this
    // function used to return the highest, and gliders juddered on every
    // gradient.)
    //
    // The ceiling only REJECTS, never selects. A branch out of reach overhead --
    // a flyover -- is skipped and the next-nearest answers instead, which is what
    // an underpass needs, and it costs no feedback because the two storeys are
    // metres apart rather than a bob apart.
    float bestD2 = 0.0f, bestY = 0.0f;
    bool  found = false;
    for (std::size_t i = 0; i < segs; ++i) {
        const glm::vec2 a = m_centerline[i], b = m_centerline[(i + 1) % n];
        const glm::vec2 ab = b - a;
        const float L2 = glm::dot(ab, ab);
        const float t = L2 > 1e-8f ? glm::clamp(glm::dot(xz - a, ab) / L2, 0.0f, 1.0f) : 0.0f;
        const glm::vec2 p = a + ab * t;
        const float d2 = glm::dot(xz - p, xz - p);
        if (d2 > lim) continue;
        float y = glm::mix(m_centerlineY[i], m_centerlineY[(i + 1) % n], t);
        // Follow the cross-fall out to where the question was actually asked. The
        // sign matches loft's: `side` is the road's right, positive bank drops
        // that edge, so the surface FALLS across a positive offset. Clamped to the
        // carriageway, or a query beyond the edge would extrapolate the tilt off
        // into the scenery.
        const float reach = edgeReach();
        if (i < m_centerlineBank.size() || reach > 1e-4f) {
            const float bk = (i < m_centerlineBank.size())
                ? glm::mix(m_centerlineBank[i], m_centerlineBank[(i + 1) % n], t)
                : 0.0f;
            const float L = std::sqrt(L2);
            const glm::vec2 dir = (L > 1e-5f) ? ab / L : glm::vec2(0.0f, 1.0f);
            const glm::vec2 rel = xz - p;
            const float half = width * 0.5f;
            const float across = rel.x * dir.y - rel.y * dir.x;   // signed, to the right
            if (bk != 0.0f)
                y -= glm::clamp(across, -half, half) * std::tan(glm::radians(bk));
            // ...and climb the raised edge beyond the carriageway, so a craft that
            // rides up the lip is lifted by it. Without this the wall is something
            // you collide with but hover straight through, which is the difference
            // between a banked track and a fence.
            if (reach > 1e-4f) {
                const float up = glm::clamp(std::abs(across) - half, 0.0f, reach);
                y += up * (edgeRise() / reach);
            }
        }
        if (y > maxY) continue;                       // out of reach overhead
        if (!found || d2 < bestD2) { bestD2 = d2; bestY = y; found = true; }
    }
    if (found) outY = bestY;
    return found;
}

void RoadSystem::clearGeometry() {
    m_mesh = fitzel::Mesh(); m_verts = 0;
    m_bridgeMesh = fitzel::Mesh(); m_bridgeVerts = 0;
    m_loopMesh = fitzel::Mesh(); m_loopVerts = 0;
    m_loops.clear();
    m_collVerts.clear(); m_collIndices.clear(); m_centerline.clear();
    m_centerlineY.clear();
    m_centerlineBank.clear();
    m_sideBatches.clear();
}

std::vector<roadside::Instance> RoadSystem::placeLine(const roadside::Line& line) const {
    if (m_centerline.size() < 2) return {}; // placements follow a committed road
    // Drape on the current terrain (which holds the graded corridor), so a post
    // stands on the ground the road actually sits on. An on-road line instead
    // stands on the carriageway, so it also gets the road's own profile (deck
    // height + cross-fall per sample) -- draping it on the terrain would bury it
    // in the asphalt and drop it off every bridge.
    auto ground = [this](float x, float z) { return m_streamer.heightAt(x, z); };
    return roadside::generate(line, m_centerline, width * 0.5f, ground,
                              m_centerlineY, m_centerlineBank);
}

void RoadSystem::rebuildSideObjects() {
    m_sideBatches.clear();
    if (m_centerline.size() < 2) return; // side objects follow a committed road
    for (const roadside::Line& line : sideLines) {
        auto inst = placeLine(line);
        if (inst.empty()) continue;
        m_sideBatches.push_back({line.model, line.scale, line.knockable,
                                 line.mass, std::move(inst)});
    }
}

void RoadSystem::rebuildDecals() {
    m_decalBatches.clear();
    if (m_centerline.size() < 2) return; // decals follow a committed road
    fitzel::Shader* lit = m_mat.shader();
    if (!lit) return;

    // The images this pass actually uses, which becomes the cache afterwards:
    // an image nobody names any more is released, one that is still named is
    // carried over rather than re-decoded.
    std::unordered_map<std::string, std::shared_ptr<fitzel::Texture>> keep;

    for (const roaddecal::Decal& d : decals) {
        fitzel::MeshData md = roaddecal::generate(d, m_centerline, m_centerlineY,
                                                  m_centerlineBank);
        if (md.vertices.empty()) continue;
        std::shared_ptr<fitzel::Texture> tex;
        if (const auto it = m_decalTex.find(d.texture); it != m_decalTex.end())
            tex = it->second;
        else
            tex = m_assetDb.loadTexture(resolveTexPath(d.texture));
        if (!tex) continue;   // the image is gone: draw nothing, not a white patch
        keep[d.texture] = tex;

        DecalBatch b{fitzel::Mesh::create(md), fitzel::Material(*lit), std::move(tex),
                     d.blend == roaddecal::Blend::Blend,
                     std::clamp(d.opacity, 0.0f, 1.0f)};

        // Every uniform the decal depends on is written HERE, including the ones
        // it wants at their default: the lit program is shared, so a uniform a
        // material never writes keeps whatever the previous draw left in it (the
        // Renderer's baseline covers some of these, but not uColorMode, uTint or
        // uAlphaCutoff -- and a decal that inherited the road's tint or the
        // terrain's colour mode would be a mystery to debug).
        b.mat.set("uColorMode", 2)
             .set("uTint", d.tint)
             .set("uRoadFade", 0.0f)      // the ribbon's edge fade is not ours
             .set("uHasNormalMap", 0)
             .set("uAlphaCutout", d.blend == roaddecal::Blend::Cutout ? 1 : 0)
             .set("uAlphaCutoff", std::clamp(d.cutoff, 0.0f, 1.0f));
        b.mat.setTexture("uTexture", *b.tex, 0);

        // Glow. The image is its own emission map, so only its painted texels
        // light up -- which is what makes a boost pad read as a lit strip on dark
        // tarmac rather than a glowing rectangle. Unit 3 is the slot the road's
        // own glow map uses; units only have to be unique within one draw.
        const float glow = std::max(d.glowStrength, 0.0f);
        b.mat.set("uEmission", d.glow)
             .set("uEmissionStrength", glow)
             .set("uHasEmissionMap", glow > 0.0f ? 1 : 0)
             .set("uEmissionUVScale", glm::vec2(1.0f)); // the patch's own 0..1 UVs
        if (glow > 0.0f) b.mat.setTexture("uEmissionMap", *b.tex, 3);

        m_decalBatches.push_back(std::move(b));
    }
    m_decalTex.swap(keep);
}

void RoadSystem::applyDecalWetness(float wetness, float waterLevel) {
    // Neither uniform is in the Renderer's baseline, so they carry over from
    // whatever drew last -- and a decal is painted ON the carriageway, so the
    // answer it wants is the carriageway's own. Markings on a soaked track are
    // soaked; left at someone else's value they read as dry paint on wet tarmac.
    for (DecalBatch& b : m_decalBatches)
        b.mat.set("uWetness", wetness).set("uWaterLevel", waterLevel);
}

void RoadSystem::rebuildCity() {
    m_city.clear();
    m_cityMeshes.clear();
    if (!cityEnabled || biomes.empty() || !cityPalettes) return;
    if (m_centerline.size() < 2) return; // the city follows a committed road
    // Terrain heights come from the LIVE streamer, which already holds the graded
    // corridor: a facade meets the ground the road actually cut, not the hillside
    // that was there before Build. Same reason rebuildSideObjects samples here.
    auto ground = [this](float x, float z) { return m_streamer.heightAt(x, z); };
    m_city = city::generate(m_centerline, m_centerlineY, width * 0.5f, biomes,
                            cityPalettes(biomes), ground, std::max(cityBudget, 0));

    // Upload the merged geometry and drop the CPU copy: a district's vertices are
    // tens of megabytes, and nothing reads them again (bake() re-runs the
    // generator from the stored params rather than scavenging triangles).
    m_cityMeshes.reserve(m_city.batches.size());
    for (city::Batch& b : m_city.batches) {
        m_cityMeshes.push_back(fitzel::Mesh::create(b.data));
        b.data = fitzel::MeshData{};
    }
}

void RoadSystem::rebuildMesh() {
    needsBuild = false;
    const Layout lo = layout();
    if (lo.center.size() < 2) { clearGeometry(); return; }

    // Loft the ribbon on the road's own profile (+ a hair), exactly as build()
    // does -- deliberately NOT on the sampled terrain height. build() grades the
    // corridor to sit a clearance (kRoadClear + the sub-cell bulge) BELOW this
    // profile so the ground can't poke through the asphalt; draping the ribbon
    // back down onto that sunk terrain (as this once did) drops it by that
    // clearance and lets the bulges show through again. That was the bug where a
    // road came up clean on Build but had the terrain poking through after a
    // reload -- a reload re-lofts the mesh through this path, not build(). The
    // profile already tracks terrain regen (layout() recomputes it from the base
    // each call), so following it here still adapts to a changed landscape, and it
    // stays level across a bridge span where the terrain drops into the gap.
    std::vector<float> h(lo.center.size());
    for (std::size_t i = 0; i < lo.center.size(); ++i)
        h[i] = lo.prof[i] + 0.06f; // lifted a touch so the ribbon reads above the ground
    loft(lo.center, h, lo.bank, lo.loops);
    buildConcrete(lo);
    buildLoops(lo);
    rebuildSideObjects();
    rebuildCity();
    rebuildDecals();
}

bool RoadSystem::build(fitzel::TerrainEditField& edit, glm::vec2& outMin,
                       glm::vec2& outMax) {
    needsBuild = false;
    vegDirty   = true; // vegetation must re-evaluate against the new road

    // 1) The road's profile over the bare terrain, and the gaps it has to span.
    const Layout L = layout();
    if (L.center.size() < 2) { clearGeometry(); return false; }

    const fitzel::TerrainSettings& s = m_streamer.settings();

    // 2) Loft the ribbon on the graded profile (lifted a hair above the surface),
    //    then hang the decks under wherever it crosses a gap.
    std::vector<float> surf(L.prof.size());
    for (std::size_t i = 0; i < L.prof.size(); ++i) surf[i] = L.prof[i] + 0.06f;
    loft(L.center, surf, L.bank, L.loops);
    buildConcrete(L);
    buildLoops(L);
    // Side objects are generated by the caller AFTER it republishes the graded
    // terrain (rebuildSideObjects), so posts drape on the corridor this build
    // just cut -- not on the pre-grade ground. loft() has set m_centerline for it.
    //
    // Decals do not wait for that: they lie on the ROAD, whose profile loft() has
    // just published, and never touch the ground the corridor is being cut into.
    rebuildDecals();

    // 3) Grade a corridor into the terrain edit field: cells within half-width get
    //    the road height; a `shoulder` band eases back to the natural ground. All
    //    deltas are stored relative to the base terrain, so the corridor is fully
    //    owned/overwritten here (repeatable, and it flattens whatever was under it).
    //    Under a bridge the grading is weighted out entirely -- the ground keeps its
    //    natural shape and the deck does the crossing.
    // The corridor is cut to the whole section, raised edges included: the lip
    // stands ABOVE the road, but its footprint is still road, and ground left
    // ungraded under it would poke through the outer half of the wall.
    const float half   = surfaceHalf();
    const float reach  = half + shoulder;
    glm::vec2 lo(L.center[0]), hi(L.center[0]);
    for (const glm::vec2& c : L.center) {
        lo = glm::min(lo, c); hi = glm::max(hi, c);
    }
    lo -= glm::vec2(reach); hi += glm::vec2(reach);

    const float cell = edit.cell;
    const int ix0 = static_cast<int>(std::floor(lo.x / cell));
    const int ix1 = static_cast<int>(std::ceil (hi.x / cell));
    const int iz0 = static_cast<int>(std::floor(lo.y / cell));
    const int iz1 = static_cast<int>(std::ceil (hi.y / cell));
    for (int iz = iz0; iz <= iz1; ++iz) {
        for (int ix = ix0; ix <= ix1; ++ix) {
            const glm::vec2 w(ix * cell, iz * cell);
            // Which stretch of road owns this cell.
            //
            // "The nearest one in plan view" is wrong wherever the road crosses
            // itself -- a figure-of-eight's underpass being the case that matters.
            // There, two branches sit over the same ground at different heights,
            // and the nearer one in XZ is not the one the ground belongs to: the
            // ground belongs to the LOWER branch, because the upper is on a deck
            // that wants no grading at all (its gradeW is 0). Deciding by distance
            // graded the underpass floor up to the flyover's height, which is
            // exactly what made a working figure-of-eight look broken.
            //
            // So: any branch whose CARRIAGEWAY covers this cell claims it, lowest
            // wins. Only if none does -- the cell is in a shoulder and nothing
            // else -- does distance decide, which is the old behaviour and the
            // right one for a road that does not cross itself.
            float bestD2 = reach * reach + 1.0f;   // for the shoulder fallback
            float roadH = 0.0f, gradeW = 1.0f, lateral = 0.0f, bankDeg = 0.0f;
            float ownD2 = 0.0f;
            bool  owned = false;                   // a carriageway covers the cell
            const float half2 = half * half;
            for (std::size_t k = 0; k + 1 < L.center.size(); ++k) {
                float t;
                const float d2 = distToSeg(w, L.center[k], L.center[k + 1], t);
                if (d2 > reach * reach) continue;
                const float h = glm::mix(L.prof[k], L.prof[k + 1], t);
                const bool  under = d2 <= half2;
                if (under ? (owned && h >= roadH)
                          : (owned || d2 >= bestD2)) continue;
                // Signed offset across the road, so a banked section grades the
                // bed it actually needs rather than a level one its low edge then
                // pokes through. Sign matches loft's `side` = (fwd.y, -fwd.x),
                // i.e. positive is the right-hand edge.
                const glm::vec2 seg = L.center[k + 1] - L.center[k];
                const float     len = glm::length(seg);
                const glm::vec2 dir = (len > 1e-5f) ? seg / len : glm::vec2(0.0f, 1.0f);
                const glm::vec2 rel = w - (L.center[k] + seg * t);
                roadH   = h;
                gradeW  = glm::mix(L.gradeW[k], L.gradeW[k + 1], t);
                bankDeg = L.bank.empty() ? 0.0f
                                         : glm::mix(L.bank[k], L.bank[k + 1], t);
                lateral = rel.x * dir.y - rel.y * dir.x; // dot(rel, (dir.y,-dir.x))
                ownD2   = d2;
                if (under) owned = true;
                else       bestD2 = d2;
            }
            if (!owned && bestD2 > reach * reach) continue; // outside the corridor
            const float d = std::sqrt(ownD2);
            const float base = terrainBaseHeight(s, w.x, w.y);
            // The surface a banked section presents at this offset. Positive bank
            // drops the RIGHT edge (loft rolls sideDir's y to -sin), and `lateral`
            // is positive to the right -- so the surface FALLS across the offset,
            // hence the minus. Getting this backwards grades the bed the mirror
            // image of the road above it, which buries one edge and leaves the
            // other hanging.
            const float surfH = roadH - lateral * std::tan(glm::radians(bankDeg));
            float target = surfH;
            float flat   = 1.0f; // 1 = flattened onto the road, 0 = natural ground
            if (d > half) {
                const float e = glm::clamp((d - half) / shoulder, 0.0f, 1.0f);
                const float k = e * e * (3.0f - 2.0f * e);
                target = glm::mix(surfH, base, k);
                flat   = 1.0f - k;
            }
            // Ease the whole corridor back to the bare ground across a bridge's
            // abutments, and let it go completely under the span itself.
            target = glm::mix(base, target, gradeW);
            flat  *= gradeW;
            // The terrain mesh adds the *exact* base noise to a bilinear sample of
            // this delta field, and its vertices are finer than our cell grid. So
            // whatever the base bulges between our nodes survives the flattening
            // and pokes up through the asphalt. Sink the node by that bulge,
            // measured as the base's linear-interpolation error at the four edge
            // midpoints: both nodes of an edge measure the same error there, so
            // their average always covers it.
            if (flat > 0.0f) target -= flat * (kRoadClear + baseBulge(s, w, cell));
            const std::int64_t key = fitzel::TerrainEditField::cellKey(ix, iz);
            // Drop the cell rather than storing a zero, so a stretch that used to be
            // an embankment and is now bridged gives its ground back (and the map
            // doesn't fill up with no-ops under every span).
            if (std::fabs(target - base) < 1e-4f) edit.deltas.erase(key);
            else                                  edit.deltas[key] = target - base;
        }
    }

    outMin = lo - glm::vec2(cell);
    outMax = hi + glm::vec2(cell);
    return true;
}

RoadSystem::Preview RoadSystem::previewGeometry() const {
    Preview pv;
    std::vector<int> ptSample;
    const std::vector<glm::vec2> center = sampleCenterlineXZ(&ptSample);
    if (center.size() < 2) return pv;
    // Draped on the *current* ground (cheap, and after a build that ground is
    // the graded corridor) plus the height offsets, so raising a point shows up
    // in the preview immediately instead of only after Build.
    const std::vector<float> off  = pointRamp(ptLift, ptSample, center.size());
    const std::vector<float> roll = pointRamp(ptBank, ptSample, center.size());
    pv.ptSample = ptSample;
    const float half = width * 0.5f;
    pv.center.reserve(center.size());
    pv.left.reserve(center.size());
    pv.right.reserve(center.size());
    for (std::size_t i = 0; i < center.size(); ++i) {
        glm::vec2 fwd = (i == 0)                 ? center[1] - center[0]
                      : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                                 : center[i + 1] - center[i - 1];
        if (glm::length(fwd) < 1e-4f) fwd = glm::vec2(0, 1);
        fwd = glm::normalize(fwd);
        const glm::vec2 side(fwd.y, -fwd.x);
        const glm::vec2 c = center[i];
        const float lift = off.empty() ? 0.0f : off[i];
        // The edges tilt with the section, exactly as loft() will build them --
        // the preview is the only thing the user sees while setting the cross-fall,
        // so it has to show the roll rather than two level rails.
        const float br = glm::radians(roll.empty() ? 0.0f : roll[i]);
        const float cb = std::cos(br), sb = std::sin(br);
        const glm::vec2 l = c - side * (half * cb);
        const glm::vec2 r = c + side * (half * cb);
        const float cy = m_streamer.heightAt(c.x, c.y) + 0.10f + lift;
        // Edges ride off the CENTRE's height once banked -- draping each rail on
        // its own ground would cancel the tilt on a slope.
        const float drop = half * sb;
        pv.center.push_back({c.x, cy, c.y});
        pv.left.push_back  ({l.x, (br != 0.0f) ? cy + drop
                                    : m_streamer.heightAt(l.x, l.y) + 0.10f + lift, l.y});
        pv.right.push_back ({r.x, (br != 0.0f) ? cy - drop
                                    : m_streamer.heightAt(r.x, r.y) + 0.10f + lift, r.y});
    }
    return pv;
}
