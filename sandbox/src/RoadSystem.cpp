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
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/world/Terrain.hpp>

#include "CameraPath.hpp" // catmull()

namespace {
// Spline sampling: aim for a sample every kSampleStep metres so a long sweeping
// span gets as many samples as it needs to read as a curve instead of a chain of
// facets, while a short one stays cheap. Clamped at both ends.
constexpr float kSampleStep = 2.0f;
constexpr int   kMinSub     = 6;
constexpr int   kMaxSub     = 128;

// Centripetal Catmull-Rom (alpha = 0.5) through b and c. The uniform variant
// (CameraPath's catmull()) assumes evenly spaced control points; where road
// points are not -- a tight corner right after a long straight is the usual case
// -- it overshoots and can loop back on itself. Knot spacing by sqrt(distance)
// removes that: the curve stays inside the control polygon and corners come out
// round rather than bulged. Falls back to the uniform form on coincident points.
glm::vec2 catmullCentripetal(const glm::vec2& p0, const glm::vec2& p1,
                             const glm::vec2& p2, const glm::vec2& p3, float t) {
    auto next = [](float ti, const glm::vec2& a, const glm::vec2& b) {
        return ti + std::sqrt(glm::length(b - a));
    };
    const float t0 = 0.0f;
    const float t1 = next(t0, p0, p1);
    const float t2 = next(t1, p1, p2);
    const float t3 = next(t2, p2, p3);
    if (t1 - t0 < 1e-5f || t2 - t1 < 1e-5f || t3 - t2 < 1e-5f)
        return catmull(p0, p1, p2, p3, t);
    const float tt = t1 + t * (t2 - t1);
    const glm::vec2 a1 = ((t1 - tt) * p0 + (tt - t0) * p1) / (t1 - t0);
    const glm::vec2 a2 = ((t2 - tt) * p1 + (tt - t1) * p2) / (t2 - t1);
    const glm::vec2 a3 = ((t3 - tt) * p2 + (tt - t2) * p3) / (t3 - t2);
    const glm::vec2 b1 = ((t2 - tt) * a1 + (tt - t0) * a2) / (t2 - t0);
    const glm::vec2 b2 = ((t3 - tt) * a2 + (tt - t1) * a3) / (t3 - t1);
    return ((t2 - tt) * b1 + (tt - t1) * b2) / (t2 - t1);
}

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
    m_mat.set("uColorMode", 2);
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

    // Bridges are cast concrete, and not user-selectable: a deck is structure, not
    // surface. uRoadFade is pinned off here on purpose -- the road turns the shared
    // lit program's edge fade on, and a material that never writes a uniform
    // inherits whatever the last one left in it.
    m_bridgeMat.set("uColorMode", 2).set("uRoadFade", 0.0f);
    m_bridgeTex = m_assetDb.loadTexture(m_texDir + "/cracked_concrete_02_diff_4k.jpg");
    if (m_bridgeTex) m_bridgeMat.setTexture("uTexture", *m_bridgeTex, 0);
}

void RoadSystem::refreshTextures(const std::string& projectDir) {
    // Remember the current selections so a project switch doesn't reset them.
    const std::string prevSel =
        (texSel >= 0 && texSel < static_cast<int>(texFiles.size()))
            ? texFiles[texSel] : std::string();
    const std::string prevNorm =
        (normSel >= 0 && normSel < static_cast<int>(normFiles.size()))
            ? normFiles[normSel] : std::string();

    texFiles.clear();
    m_texPaths.clear();
    normFiles.clear();
    m_normPaths.clear();
    std::unordered_set<std::string> seen;     // dedupe by display name
    std::unordered_set<std::string> seenNorm;

    auto add = [&](const std::filesystem::path& p) {
        const std::string name = p.filename().string();
        // A file is one or the other: isRoadAlbedo already rejects "_nor".
        if (isRoadNormal(name)) {
            if (!seenNorm.insert(name).second) return;
            normFiles.push_back(name);
            m_normPaths.push_back(p.generic_string());
            return;
        }
        if (!isRoadAlbedo(name) || !seen.insert(name).second) return;
        texFiles.push_back(name);
        m_texPaths.push_back(p.generic_string());
    };

    // Built-in content textures (flat scan, as before).
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(m_texDir, ec))
        add(e.path());
    // Project-local textures (recursive), so surfaces dropped into the open
    // project show up too. Names already present in content are kept (not shadowed).
    if (!projectDir.empty() && std::filesystem::is_directory(projectDir, ec))
        for (const auto& e :
             std::filesystem::recursive_directory_iterator(projectDir, ec))
            if (!e.is_directory()) add(e.path());

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

    // Restore the selections by name (else fall back: first surface, no normal).
    texSel = 0;
    for (int i = 0; i < static_cast<int>(texFiles.size()); ++i)
        if (texFiles[i] == prevSel) { texSel = i; break; }
    normSel = -1;
    for (int i = 0; i < static_cast<int>(normFiles.size()); ++i)
        if (normFiles[i] == prevNorm) { normSel = i; break; }
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

void RoadSystem::insertPoint(int at, glm::vec2 p, float lift) {
    at = std::clamp(at, 0, static_cast<int>(roadPts.size()));
    ptLift.resize(roadPts.size(), 0.0f); // heal a short array before indexing it
    roadPts.insert(roadPts.begin() + at, p);
    ptLift.insert(ptLift.begin() + at, lift);
    needsBuild = true;
}

void RoadSystem::erasePoint(int at) {
    if (at < 0 || at >= static_cast<int>(roadPts.size())) return;
    ptLift.resize(roadPts.size(), 0.0f);
    roadPts.erase(roadPts.begin() + at);
    ptLift.erase(ptLift.begin() + at);
    needsBuild = true;
}

void RoadSystem::clearPoints() {
    roadPts.clear();
    ptLift.clear();
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

    nlohmann::json bridges_ = nlohmann::json::array();
    for (const BridgeSpec& b : bridges) bridges_.push_back({b.a, b.b});

    nlohmann::json side_ = nlohmann::json::array();
    for (const roadside::Line& l : sideLines)
        side_.push_back({
            {"enabled",  l.enabled},
            {"kind",     static_cast<int>(l.kind)},
            {"model",    l.model},
            {"side",     l.side},
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

    j = {
        {"points",    rs.str()},
        {"lifts",     ls.str()},
        {"closed",    closed},
        {"enabled",   enabled},
        {"width",     width},
        {"texTile",   texTile},
        {"fadeWidth", fadeWidth},
        {"rainRings", rainRings},
        {"grade",     grade},
        {"shoulder",  shoulder},
        // The surface goes by name, not index: the texture list is rebuilt from
        // disk each run, so an index would point somewhere else next time.
        {"surface",   (texSel >= 0 && texSel < static_cast<int>(texFiles.size()))
                          ? texFiles[texSel] : std::string()},
        {"normal",    (normSel >= 0 && normSel < static_cast<int>(normFiles.size()))
                          ? normFiles[normSel] : std::string()},
        {"bridges",   bridges_},
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
    };
}

void RoadSystem::load(const nlohmann::json& j) {
    // Every field defaults to what a fresh road has, so a scene saved before a
    // param existed loads as the road it was built as.
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
    // A scene from before heights (or a truncated/overlong blob) is filled out
    // to match the points, so nothing downstream has to bounds-check the pair.
    ptLift.resize(roadPts.size(), 0.0f);
    closed    = j.value("closed",    false);
    enabled   = j.value("enabled",   true);
    width     = j.value("width",     5.0f);
    texTile   = j.value("texTile",   8.0f);
    fadeWidth = j.value("fadeWidth", 0.0f);
    rainRings = j.value("rainRings", 1.0f);
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

    // Bridges are absent from scenes saved before they existed -- those roads
    // simply have none, which is exactly how they were built.
    bridges.clear();
    if (j.contains("bridges") && j["bridges"].is_array())
        for (const auto& b : j["bridges"])
            if (b.is_array() && b.size() == 2)
                bridges.push_back({b[0].get<int>(), b[1].get<int>()});

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
}

std::vector<glm::vec2> RoadSystem::sampleCenterlineXZ(
        std::vector<int>* ptSample) const {
    std::vector<glm::vec2> center;
    if (ptSample) ptSample->clear();
    const int n = static_cast<int>(roadPts.size());
    if (n < 2) return center;
    // A closed loop needs >= 3 points to be more than a back-and-forth. When
    // looping, control points wrap around (modulo n) so the tangents are
    // continuous across the seam; the extra segment n-1 -> 0 closes the ring.
    const bool loop = closed && n >= 3;
    auto pt = [&](int i) -> glm::vec2 {
        if (loop) return roadPts[((i % n) + n) % n];
        // Open ends: mirror a phantom point through the endpoint instead of
        // repeating it. A repeated point has zero knot spacing (degenerate for
        // the centripetal form) and flattens the first/last span's tangent;
        // mirroring lets the road leave its end point along the curve it is on.
        if (i < 0)      return 2.0f * roadPts[0] - roadPts[1];
        if (i > n - 1)  return 2.0f * roadPts[n - 1] - roadPts[n - 2];
        return roadPts[i];
    };
    const int segs = loop ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const glm::vec2 p0 = pt(i - 1);
        const glm::vec2 p1 = pt(i);
        const glm::vec2 p2 = pt(i + 1);
        const glm::vec2 p3 = pt(i + 2);
        // Sample count from this span's own length, so curvature is resolved the
        // same everywhere regardless of how far apart the user set the points.
        const int sub = std::clamp(
            static_cast<int>(std::lround(glm::length(p2 - p1) / kSampleStep)),
            kMinSub, kMaxSub);
        // Control point i is the span's first sample -- recorded for the bridge
        // specs, which name their ends by control-point index.
        if (ptSample) ptSample->push_back(static_cast<int>(center.size()));
        // Each span drops its final sample (it repeats the next span's first);
        // only the very last segment keeps it, to terminate the open line or to
        // land back on the start point and close the loop.
        const int last = (i == segs - 1) ? sub : sub - 1;
        for (int s = 0; s <= last; ++s)
            center.push_back(catmullCentripetal(p0, p1, p2, p3,
                                                static_cast<float>(s) / sub));
    }
    // The line's final sample closes out the last control point (the loop's
    // start point, or the open line's end point).
    if (ptSample) ptSample->push_back(static_cast<int>(center.size()) - 1);
    return center;
}

void RoadSystem::loft(const std::vector<glm::vec2>& center,
                      const std::vector<float>& height) {
    fitzel::MeshData md;
    m_centerline = center; // flat centre for vegetation masking

    const float half = width * 0.5f;
    float vlen = 0.0f;
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
        const float v = vlen / texTile;
        const glm::vec3 Lp(center[i].x - side.x * half, height[i],
                           center[i].y - side.y * half);
        const glm::vec3 Rp(center[i].x + side.x * half, height[i],
                           center[i].y + side.y * half);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        md.vertices.push_back({Lp, up, {0.0f, v}});
        md.vertices.push_back({Rp, up, {width / texTile, v}});
    }
    // Two triangles per rung, wound CCW-from-above (front faces up).
    for (std::uint32_t i = 0; i + 1 < center.size(); ++i) {
        const std::uint32_t a = 2 * i;
        md.indices.insert(md.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
    }
    m_mesh  = fitzel::Mesh::create(md);
    m_verts = static_cast<int>(md.vertices.size());

    // Keep the CPU geometry for the physics mesh collider (Play mode).
    m_collVerts.clear();
    m_collVerts.reserve(md.vertices.size());
    for (const fitzel::Vertex& vtx : md.vertices) m_collVerts.push_back(vtx.position);
    m_collIndices = md.indices;
}

void RoadSystem::buildBridges(const Layout& lo) {
    fitzel::MeshData md;
    roadbridge::build(lo.center, lo.prof, lo.ground, lo.spans, width, bridgeStyle, md);
    m_bridgeVerts = static_cast<int>(md.vertices.size());
    if (md.indices.empty()) { m_bridgeMesh = fitzel::Mesh(); return; }
    m_bridgeMesh = fitzel::Mesh::create(md);

    // Bridges collide as part of the one static road mesh: the deck because it is
    // the only ground there is up here, the parapets because they are what keep a
    // car from driving off the side.
    const auto base = static_cast<std::uint32_t>(m_collVerts.size());
    for (const fitzel::Vertex& vtx : md.vertices) m_collVerts.push_back(vtx.position);
    m_collIndices.reserve(m_collIndices.size() + md.indices.size());
    for (std::uint32_t idx : md.indices) m_collIndices.push_back(base + idx);
}

std::vector<float> RoadSystem::liftRamp(const std::vector<int>& ptSample,
                                        std::size_t samples) const {
    if (samples == 0 || std::none_of(ptLift.begin(), ptLift.end(),
                                     [](float v) { return v != 0.0f; }))
        return {}; // no heights set: the caller skips the whole addition
    const int n = static_cast<int>(roadPts.size());
    std::vector<float> off(samples, 0.0f);
    // Linear between control points -- predictable, and unlike a spline it cannot
    // overshoot and dip a raised stretch back into the ground it is meant to
    // clear.
    for (std::size_t k = 0; k + 1 < ptSample.size(); ++k) {
        const int a = ptSample[k], b = ptSample[k + 1];
        if (b <= a) continue;
        const float la = liftOf(static_cast<int>(k));
        // The closing entry wraps to point 0 on a loop and repeats the last point
        // on an open road; the modulo covers both.
        const float lb = liftOf(n > 0 ? static_cast<int>(k + 1) % n : 0);
        for (int i = a; i <= b && i < static_cast<int>(off.size()); ++i)
            off[i] = glm::mix(la, lb, static_cast<float>(i - a) / (b - a));
    }
    // Round off the kink each control point leaves in the ramp. Ends are held, so
    // a road that starts or ends lifted keeps that height exactly.
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
    const std::vector<float> off = liftRamp(ptSample, lo.center.size());
    if (!off.empty())
        for (std::size_t i = 0; i < lo.prof.size(); ++i) lo.prof[i] += off[i];

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
    // The chords meet the road at an angle; round those two kinks off so a bridge
    // entrance isn't a bump. A straight chord is a fixed point of this filter, so
    // only the tangents move.
    if (!lo.spans.empty()) smooth(lo.prof, passesFor(5.0f));
    return lo;
}

void RoadSystem::clearGeometry() {
    m_mesh = fitzel::Mesh(); m_verts = 0;
    m_bridgeMesh = fitzel::Mesh(); m_bridgeVerts = 0;
    m_collVerts.clear(); m_collIndices.clear(); m_centerline.clear();
    m_sideBatches.clear();
}

void RoadSystem::rebuildSideObjects() {
    m_sideBatches.clear();
    if (m_centerline.size() < 2) return; // side objects follow a committed road
    // Drape on the current terrain (which holds the graded corridor), so a post
    // stands on the ground the road actually sits on.
    auto ground = [this](float x, float z) { return m_streamer.heightAt(x, z); };
    const float half = width * 0.5f;
    for (const roadside::Line& line : sideLines) {
        auto inst = roadside::generate(line, m_centerline, half, ground);
        if (inst.empty()) continue;
        m_sideBatches.push_back({line.model, line.scale, line.knockable,
                                 line.mass, std::move(inst)});
    }
}

void RoadSystem::rebuildMesh() {
    needsBuild = false;
    const Layout lo = layout();
    if (lo.center.size() < 2) { clearGeometry(); return; }

    // Off a deck, drape on the current terrain (which already holds the graded
    // corridor after a build/scene-load) so the road follows ground that has been
    // sculpted or re-generated since. On a deck, the road stays on its profile:
    // the terrain under a span is untouched and still ramping across the abutment,
    // so draping there would sink the road into the gap it is supposed to cross.
    // The two agree at a span's ends, where the grading has brought the ground all
    // the way up to the road -- so the seam is invisible.
    std::vector<char> onDeck(lo.center.size(), 0);
    for (const roadbridge::Span& sp : lo.spans)
        for (int i = sp.begin; i <= sp.end; ++i) onDeck[i] = 1;

    std::vector<float> h(lo.center.size());
    for (std::size_t i = 0; i < lo.center.size(); ++i)
        h[i] = (onDeck[i] ? lo.prof[i]
                          : m_streamer.heightAt(lo.center[i].x, lo.center[i].y))
               + 0.06f; // lifted a touch so the ribbon reads above the ground
    loft(lo.center, h);
    buildBridges(lo);
    rebuildSideObjects();
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
    loft(L.center, surf);
    buildBridges(L);
    // Side objects are generated by the caller AFTER it republishes the graded
    // terrain (rebuildSideObjects), so posts drape on the corridor this build
    // just cut -- not on the pre-grade ground. loft() has set m_centerline for it.

    // 3) Grade a corridor into the terrain edit field: cells within half-width get
    //    the road height; a `shoulder` band eases back to the natural ground. All
    //    deltas are stored relative to the base terrain, so the corridor is fully
    //    owned/overwritten here (repeatable, and it flattens whatever was under it).
    //    Under a bridge the grading is weighted out entirely -- the ground keeps its
    //    natural shape and the deck does the crossing.
    const float half   = width * 0.5f;
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
            // Nearest point on the centreline + the road height and grading weight
            // there.
            float bestD2 = reach * reach + 1.0f, roadH = 0.0f, gradeW = 1.0f;
            for (std::size_t k = 0; k + 1 < L.center.size(); ++k) {
                float t;
                const float d2 = distToSeg(w, L.center[k], L.center[k + 1], t);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    roadH  = glm::mix(L.prof[k], L.prof[k + 1], t);
                    gradeW = glm::mix(L.gradeW[k], L.gradeW[k + 1], t);
                }
            }
            const float d = std::sqrt(bestD2);
            if (d > reach) continue; // outside the corridor -> leave the ground be
            const float base = terrainBaseHeight(s, w.x, w.y);
            float target = roadH;
            float flat   = 1.0f; // 1 = flattened onto the road, 0 = natural ground
            if (d > half) {
                const float e = glm::clamp((d - half) / shoulder, 0.0f, 1.0f);
                const float k = e * e * (3.0f - 2.0f * e);
                target = glm::mix(roadH, base, k);
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
    const std::vector<float> off = liftRamp(ptSample, center.size());
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
        const glm::vec2 l = c - side * half;
        const glm::vec2 r = c + side * half;
        const float lift = off.empty() ? 0.0f : off[i];
        pv.center.push_back({c.x, m_streamer.heightAt(c.x, c.y) + 0.10f + lift, c.y});
        pv.left.push_back  ({l.x, m_streamer.heightAt(l.x, l.y) + 0.10f + lift, l.y});
        pv.right.push_back ({r.x, m_streamer.heightAt(r.x, r.y) + 0.10f + lift, r.y});
    }
    return pv;
}
