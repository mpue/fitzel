#include "BridgeGen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include "SplineGenDetail.hpp"

// The bridge generator. Everything is laid out on the DECK LINE -- the top of the
// running surface along the centre, which SplineSystem has already made straight
// from end to end -- and hung from it or stood under it:
//
//   deck      slab + girder swept along the line, parapets or railings on it
//   supports  abutments under the ends, piers at an even spacing down to ground
//   arch      one rib pair over or under the whole deck, or masonry arches
//             filling every span between the piers
//   truss     a Pratt truss either side, braced across the top
//   cables    towers, then main cables + hangers or straight stays
//
// Those are independent layers, not bridge types. A preset switches on the ones
// that make it the bridge it is named after (see bridgePreset at the bottom).
namespace splinegen::detail {
namespace {

using fitzel::AssetId;
using BStyle = bridgegen::Style;

const glm::vec3 kUp(0.0f, 1.0f, 0.0f);

// Clamp the rule into something that can be built -- inside the generator, so a
// hand-edited scene loads instead of hanging on a 0 m hanger spacing.
BStyle sane(const BStyle& in) {
    BStyle s = in;
    s.width       = glm::clamp(s.width, 0.5f, 80.0f);
    s.thick       = glm::clamp(s.thick, 0.05f, 10.0f);
    s.girder      = glm::clamp(s.girder, 0.0f, 20.0f);
    s.camber      = glm::clamp(s.camber, -50.0f, 50.0f);
    s.railHeight  = glm::clamp(s.railHeight, 0.1f, 10.0f);
    s.railThick   = glm::clamp(s.railThick, 0.02f, 3.0f);
    s.postEvery   = std::max(s.postEvery, 0.3f);
    s.pierEvery   = std::max(s.pierEvery, 0.0f);
    s.pierCols    = glm::clamp(s.pierCols, 1, 8);
    s.pierWidth   = glm::clamp(s.pierWidth, 0.1f, 40.0f);
    s.pierDepth   = glm::clamp(s.pierDepth, 0.1f, 80.0f);
    s.abutment    = glm::clamp(s.abutment, 0.0f, 100.0f);
    s.archRise    = glm::clamp(s.archRise, 0.5f, 400.0f);
    s.archRib     = glm::clamp(s.archRib, 0.1f, 20.0f);
    s.archInset   = std::max(s.archInset, 0.0f);
    s.hangerEvery = std::max(s.hangerEvery, 0.5f);
    s.hangerThick = glm::clamp(s.hangerThick, 0.02f, 5.0f);
    s.trussHeight = glm::clamp(s.trussHeight, 0.0f, 60.0f);
    s.trussPanel  = std::max(s.trussPanel, 1.0f);
    s.trussBar    = glm::clamp(s.trussBar, 0.05f, 5.0f);
    s.towerHeight = glm::clamp(s.towerHeight, 2.0f, 600.0f);
    s.towerAt     = glm::clamp(s.towerAt, 0.02f, 0.5f);
    s.towerWidth  = glm::clamp(s.towerWidth, 0.2f, 30.0f);
    s.cableSag    = glm::clamp(s.cableSag, 0.3f, 400.0f);
    s.cableThick  = glm::clamp(s.cableThick, 0.02f, 5.0f);
    s.stayEvery   = std::max(s.stayEvery, 1.0f);
    return s;
}

// --- The deck line, at any station -------------------------------------------

// The frame at `st` metres along the deck, interpolated between samples. Pier
// and hanger positions come from the bridge's own proportions (spans, panels),
// not from where the spline happened to be sampled.
Frame frameAt(const std::vector<Frame>& f, float st) {
    if (st <= f.front().station) return f.front();
    if (st >= f.back().station)  return f.back();
    const auto it = std::upper_bound(f.begin(), f.end(), st,
                                     [](float v, const Frame& fr) { return v < fr.station; });
    const Frame& b = *it;
    const Frame& a = *(it - 1);
    const float u = (st - a.station) / std::max(b.station - a.station, 1e-5f);
    Frame o;
    o.p       = glm::mix(a.p, b.p, u);
    o.t       = glm::normalize(glm::mix(a.t, b.t, u) + glm::vec3(1e-6f, 0.0f, 0.0f));
    o.r       = glm::normalize(glm::cross(kUp, o.t));
    o.station = st;
    o.yaw     = std::atan2(o.t.x, o.t.z);
    return o;
}

// --- Solids of any orientation -----------------------------------------------

// Four corners of a section, in order around it. A member is a run of these.
using Ring = std::array<glm::vec3, 4>;

// The section around `c`: `side` and `up` are its two axes (unit), `hs`/`hu` the
// half-extents along them.
Ring ring(const glm::vec3& c, const glm::vec3& side, const glm::vec3& up, float hs, float hu) {
    return {c - side * hs - up * hu, c + side * hs - up * hu,
            c + side * hs + up * hu, c - side * hs + up * hu};
}

// Loft a four-sided member through `rows`: one strip per side, capped at both
// ends. The general tool here -- a curved arch rib, a sagging cable, a spandrel
// whose underside is an arch and a straight beam (two rows) are all this.
//
// Normals are worked out from the geometry and turned to face away from the
// member's own axis, so the caller never has to think about winding. Along the
// member they are averaged between neighbouring quads, so a rib curves smoothly
// instead of showing its facets; round the section they stay hard.
void loft(Slot& sl, const std::vector<Ring>& rows, float tile) {
    const std::size_t n = rows.size();
    if (n < 2) return;
    const float t = std::max(tile, 0.05f);
    std::vector<glm::vec3> mid(n);
    for (std::size_t i = 0; i < n; ++i)
        mid[i] = (rows[i][0] + rows[i][1] + rows[i][2] + rows[i][3]) * 0.25f;

    auto grow = [&](const glm::vec3& p) {
        sl.lo = glm::min(sl.lo, p);
        sl.hi = glm::max(sl.hi, p);
    };

    for (int k = 0; k < 4; ++k) {
        const int k1 = (k + 1) % 4;
        // Outward face normal of each quad on this side.
        std::vector<glm::vec3> qn(n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const glm::vec3 a = rows[i][k], b = rows[i][k1], d = rows[i + 1][k];
            glm::vec3 nrm = glm::cross(b - a, d - a);
            const glm::vec3 out = (a + b + rows[i + 1][k1] + d) * 0.25f -
                                  (mid[i] + mid[i + 1]) * 0.5f;
            if (glm::dot(nrm, out) < 0.0f) nrm = -nrm;
            const float l = glm::length(nrm);
            qn[i] = l > 1e-9f ? nrm / l : glm::normalize(out + glm::vec3(0.0f, 1e-6f, 0.0f));
        }
        // Per-row normal: the average of the quads either side.
        const auto base = static_cast<std::uint32_t>(sl.data.vertices.size());
        float along = 0.0f;
        const float across = glm::length(rows[0][k1] - rows[0][k]);
        for (std::size_t i = 0; i < n; ++i) {
            if (i > 0) along += glm::length((rows[i][k] + rows[i][k1]) -
                                            (rows[i - 1][k] + rows[i - 1][k1])) * 0.5f;
            glm::vec3 nrm = (i == 0) ? qn[0] : (i + 1 == n) ? qn[n - 2] : qn[i - 1] + qn[i];
            nrm = glm::normalize(nrm + glm::vec3(0.0f, 1e-7f, 0.0f));
            for (int e = 0; e < 2; ++e) {
                fitzel::Vertex v;
                v.position = rows[i][e == 0 ? k : k1];
                v.normal   = nrm;
                v.uv       = glm::vec2(along / t, (e == 0 ? 0.0f : across) / t);
                grow(v.position);
                sl.data.vertices.push_back(v);
            }
        }
        // Wind each quad so its geometric normal agrees with the outward one.
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const std::uint32_t a = base + static_cast<std::uint32_t>(i * 2);
            const std::uint32_t b = a + 1, c = a + 3, d = a + 2;
            const glm::vec3 g = glm::cross(rows[i][k1] - rows[i][k], rows[i + 1][k1] - rows[i][k]);
            if (glm::dot(g, qn[i]) >= 0.0f) {
                for (std::uint32_t idx : {a, b, c, a, c, d}) sl.data.indices.push_back(idx);
            } else {
                for (std::uint32_t idx : {a, c, b, a, d, c}) sl.data.indices.push_back(idx);
            }
        }
    }

    // End caps, facing away from the member.
    auto cap = [&](std::size_t i, std::size_t inner) {
        const Ring& r = rows[i];
        glm::vec3 nrm = glm::cross(r[1] - r[0], r[2] - r[0]);
        const bool flip = glm::dot(nrm, mid[i] - mid[inner]) < 0.0f;
        if (flip) nrm = -nrm;
        const float l = glm::length(nrm);
        if (l < 1e-9f) return;
        nrm /= l;
        const auto base = static_cast<std::uint32_t>(sl.data.vertices.size());
        for (int e = 0; e < 4; ++e) {
            fitzel::Vertex v;
            v.position = r[e];
            v.normal   = nrm;
            v.uv = glm::vec2(glm::length(r[1] - r[0]) * (e == 1 || e == 2),
                             glm::length(r[3] - r[0]) * (e >= 2)) / t;
            grow(v.position);
            sl.data.vertices.push_back(v);
        }
        if (!flip) for (std::uint32_t idx : {0u, 1u, 2u, 0u, 2u, 3u}) sl.data.indices.push_back(base + idx);
        else       for (std::uint32_t idx : {0u, 2u, 1u, 0u, 3u, 2u}) sl.data.indices.push_back(base + idx);
    };
    cap(0, 1);
    cap(n - 1, n - 2);
}

// A straight member from `a` to `b`, `w` wide (kept horizontal) and `h` deep.
void beam(Slot& sl, const glm::vec3& a, const glm::vec3& b, float w, float h, float tile) {
    const glm::vec3 d = b - a;
    const float len = glm::length(d);
    if (len < 1e-4f) return;
    const glm::vec3 fwd = d / len;
    glm::vec3 side = glm::cross(kUp, fwd);
    side = glm::dot(side, side) < 1e-8f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::normalize(side);
    const glm::vec3 up = glm::cross(fwd, side);
    loft(sl, {ring(a, side, up, w * 0.5f, h * 0.5f), ring(b, side, up, w * 0.5f, h * 0.5f)}, tile);
}

// A member along a curve given as points: section axes are the curve's own
// (side stays horizontal), so a rib or a cable keeps its shape round the bend.
void curve(Slot& sl, const std::vector<glm::vec3>& pts, float w, float h, float tile) {
    const std::size_t n = pts.size();
    if (n < 2) return;
    std::vector<Ring> rows(n);
    for (std::size_t i = 0; i < n; ++i) {
        glm::vec3 fwd = pts[std::min(i + 1, n - 1)] - pts[i > 0 ? i - 1 : 0];
        fwd = glm::normalize(fwd + glm::vec3(0.0f, 0.0f, 1e-7f));
        glm::vec3 side = glm::cross(kUp, fwd);
        side = glm::dot(side, side) < 1e-8f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::normalize(side);
        rows[i] = ring(pts[i], side, glm::cross(fwd, side), w * 0.5f, h * 0.5f);
    }
    loft(sl, rows, tile);
}

// An upright block from `bottom` to `top` metres in world Y, centred on `at`
// in plan, turned to `yaw`. Piers, footings, abutments, tower legs.
void block(Slot& sl, const glm::vec3& at, float bottom, float top, float halfAlong,
           float halfAcross, float yaw, float tile) {
    if (top - bottom < 0.05f) return;
    appendBox(sl, glm::vec3(at.x, (bottom + top) * 0.5f, at.z),
              glm::vec3(halfAcross, (top - bottom) * 0.5f, halfAlong), yaw, tile);
}

// n evenly spaced stations strictly inside (a, b), `every` metres apart at most.
std::vector<float> evenly(float a, float b, float every) {
    std::vector<float> out;
    if (every <= 0.0f || b - a <= every * 0.5f) return out;
    const int n = std::max(1, static_cast<int>(std::lround((b - a) / every)));
    for (int k = 1; k < n; ++k) out.push_back(a + (b - a) * static_cast<float>(k) / static_cast<float>(n));
    return out;
}

} // namespace

// The bridge presets. Each sets only what makes it that bridge; the rest is
// the Style defaults (an 8 m concrete deck with parapets).
void bridgePreset(Preset p, Style& s) {
    bridgegen::Style& b = s.bridge;
    b = bridgegen::Style{};
    s.sink    = 1.0f;       // footings go well into the ground
    s.texTile = 3.0f;
    s.colorA  = {0.62f, 0.61f, 0.58f};   // concrete deck
    s.colorB  = {0.36f, 0.40f, 0.44f};   // painted steel
    s.colorC  = {0.58f, 0.57f, 0.54f};   // concrete piers
    using bridgegen::Arch;
    using bridgegen::Cable;
    using bridgegen::Rail;
    switch (p) {
        case Preset::BeamBridge:   // the motorway overpass: a deck on wall piers
            break;
        case Preset::Footbridge:   // timber, narrow, on twin posts
            b.width = 2.4f; b.thick = 0.25f; b.girder = 0.35f;
            b.rail = Rail::Railing; b.railHeight = 1.1f; b.railThick = 0.1f;
            b.postEvery = 1.8f;
            b.pierEvery = 6.0f; b.pierCols = 2; b.pierWidth = 0.25f;
            b.abutment = 1.2f;
            s.colorA = {0.45f, 0.33f, 0.22f};
            s.colorB = {0.38f, 0.28f, 0.19f};
            s.colorC = {0.34f, 0.26f, 0.18f};
            s.sink = 0.5f; s.texTile = 1.5f;
            break;
        case Preset::Viaduct:      // stone arches between tall piers
            b.width = 6.5f; b.thick = 0.5f; b.girder = 0.0f;
            b.rail = Rail::Parapet; b.railHeight = 1.0f; b.railThick = 0.45f;
            b.pierEvery = 16.0f; b.pierWidth = 2.6f; b.pierDepth = 6.5f;
            b.arch = Arch::Masonry; b.archRise = 8.0f; b.archRib = 1.1f;
            b.abutment = 5.0f;
            s.colorA = {0.60f, 0.55f, 0.47f};
            s.colorB = {0.40f, 0.40f, 0.40f};
            s.colorC = {0.56f, 0.51f, 0.44f};
            s.texTile = 2.5f;
            break;
        case Preset::DeckArch:     // one concrete arch under the deck
            b.pierEvery = 0.0f;
            b.arch = Arch::Below; b.archRise = 22.0f; b.archRib = 1.8f;
            b.archInset = 6.0f; b.hangerEvery = 7.0f; b.hangerThick = 0.9f;
            b.girder = 0.8f;
            s.colorB = s.colorA;   // concrete ribs
            break;
        case Preset::TiedArch:     // a steel arch over the deck, hangers down
            b.pierEvery = 0.0f; b.girder = 1.4f;
            b.arch = Arch::Above; b.archRise = 16.0f; b.archRib = 1.3f;
            b.hangerEvery = 5.0f; b.hangerThick = 0.12f;
            b.rail = Rail::Railing; b.railThick = 0.12f;
            s.colorB = {0.55f, 0.18f, 0.14f};  // oxide red
            break;
        case Preset::TrussBridge:  // a through truss, railway-era
            b.width = 7.0f; b.girder = 0.6f; b.pierEvery = 40.0f;
            b.pierDepth = 7.5f;
            b.trussHeight = 7.0f; b.trussPanel = 5.0f; b.trussBar = 0.4f;
            b.rail = Rail::None;
            s.colorB = {0.22f, 0.30f, 0.26f};  // bridge green
            break;
        case Preset::Suspension:
            b.width = 12.0f; b.girder = 2.2f; b.pierEvery = 0.0f;
            b.cable = Cable::Suspension; b.towerHeight = 45.0f; b.towerAt = 0.2f;
            b.towerWidth = 3.0f; b.cableSag = 2.0f; b.cableThick = 0.6f;
            b.hangerEvery = 6.0f; b.hangerThick = 0.1f;
            b.rail = Rail::Railing; b.railThick = 0.15f;
            s.colorB = {0.72f, 0.28f, 0.16f};  // international orange
            s.colorC = {0.72f, 0.28f, 0.16f};
            break;
        case Preset::CableStayed:
            b.width = 14.0f; b.girder = 1.6f; b.pierEvery = 0.0f;
            b.cable = Cable::Stayed; b.towerHeight = 50.0f; b.towerAt = 0.5f;
            b.towerWidth = 3.2f; b.cableThick = 0.22f; b.stayEvery = 9.0f;
            b.rail = Rail::Railing; b.railThick = 0.15f;
            s.colorB = {0.90f, 0.90f, 0.88f};  // white stays
            break;
        default: break;
    }
}

} // namespace splinegen::detail

namespace splinegen {

using namespace detail;

Result generateBridge(const Style& sIn, const std::vector<glm::vec3>& deck,
                      const std::function<float(float, float)>& groundAt,
                      const Palette& pal, int maxPieces) {
    Result res;
    if (deck.size() < 2) return res;
    const bridgegen::Style b = sane(sIn.bridge);
    const float tile = std::max(sIn.texTile, 0.05f);
    const float sink = glm::clamp(sIn.sink, 0.0f, 10.0f);
    auto ground = [&](const glm::vec3& p) { return groundAt ? groundAt(p.x, p.z) : 0.0f; };

    const std::vector<Frame> f = makeFrames(deck, false);
    if (f.size() < 2) return res;
    const float len = f.back().station;
    res.length = len;
    if (len < 0.5f) return res;
    int budget = std::max(maxPieces, 1);

    Slot slot[3];   // deck, steel (structure), piers (masonry)
    Slot& sDeck = slot[0];
    Slot& sSteel = slot[1];
    Slot& sPier = slot[2];
    const float hw     = b.width * 0.5f;
    const float soffit = -(b.thick + b.girder);   // underside, below the deck line
    const std::size_t last = f.size() - 1;
    auto deckY = [&](float st) { return frameAt(f, st).p.y; };
    auto piece = [&]() { --budget; ++res.pieces; return budget > 0; };

    // --- Deck ---------------------------------------------------------------
    sweep(sDeck, f, 0, last, rectProfile(hw, -b.thick, 0.0f), 0.0f, tile, true, true);
    if (b.girder > 0.0f)
        sweep(sDeck, f, 0, last, rectProfile(hw * 0.62f, soffit, -b.thick), 0.0f, tile,
              true, true);

    // --- Edges --------------------------------------------------------------
    const float railLat = hw - b.railThick * 0.5f;
    if (b.rail == bridgegen::Rail::Parapet) {
        for (float side : {-1.0f, 1.0f})
            sweep(sDeck, f, 0, last, rectProfile(b.railThick * 0.5f, -0.02f, b.railHeight),
                  side * railLat, tile, true, true);
    } else if (b.rail == bridgegen::Rail::Railing) {
        const float bar = std::min(b.railThick, 0.12f);
        for (float side : {-1.0f, 1.0f}) {
            for (const Stop& st : stopsIn(f, 0, last, b.postEvery, budget)) {
                const glm::vec3 r(std::cos(st.yaw), 0.0f, -std::sin(st.yaw));
                block(sSteel, st.p + r * (side * railLat), st.p.y, st.p.y + b.railHeight,
                      b.railThick * 0.5f, b.railThick * 0.5f, st.yaw, tile);
                ++res.pieces;
            }
            for (float h : {b.railHeight - bar * 0.5f, b.railHeight * 0.5f})
                sweep(sSteel, f, 0, last, rectProfile(bar * 0.5f, h - bar * 0.5f, h + bar * 0.5f),
                      side * railLat, tile, true, true);
        }
        // The posts at the very ends, which the spacing walk may stop short of.
        for (const Frame* e : {&f.front(), &f.back()})
            for (float side : {-1.0f, 1.0f})
                block(sSteel, e->p + e->r * (side * railLat), e->p.y, e->p.y + b.railHeight,
                      b.railThick * 0.5f, b.railThick * 0.5f, e->yaw, tile);
    }

    // --- Towers (placed first: piers keep out of their way) ------------------
    std::vector<float> towers;
    if (b.cable != bridgegen::Cable::None) {
        if (b.towerAt >= 0.49f) towers = {len * 0.5f};
        else                    towers = {len * b.towerAt, len * (1.0f - b.towerAt)};
    }
    const float legLat = hw + b.towerWidth * 0.5f + 0.1f;   // cables hang in this plane

    // --- Abutments and piers --------------------------------------------------
    if (b.abutment > 0.0f) {
        const float a = std::min(b.abutment, len * 0.5f);
        for (float st : {a * 0.5f, len - a * 0.5f}) {
            const Frame fr = frameAt(f, st);
            const float lo = std::min({ground(frameAt(f, st - a * 0.5f).p),
                                       ground(frameAt(f, st + a * 0.5f).p), ground(fr.p)});
            block(sPier, fr.p, lo - sink, fr.p.y - b.thick, a * 0.5f, hw + 0.3f, fr.yaw, tile);
            ++res.pieces;
        }
    }

    // Where the single arch springs, if there is one.
    const float archA = std::min(b.archInset, len * 0.45f);
    const float archB = len - archA;
    const bool  archBelow = b.arch == bridgegen::Arch::Below;

    std::vector<float> piers = evenly(0.0f, len, b.pierEvery);
    piers.erase(std::remove_if(piers.begin(), piers.end(), [&](float st) {
        for (float t : towers)
            if (std::abs(st - t) < b.towerWidth + b.pierWidth) return true;
        // An arch under the deck carries it: no piers in its reach.
        return archBelow && st > archA && st < archB;
    }), piers.end());

    for (float st : piers) {
        const Frame fr = frameAt(f, st);
        const float top = fr.p.y + soffit;
        if (b.pierCols <= 1) {
            const float lo = ground(fr.p);
            if (top - lo < 0.3f) continue;
            block(sPier, fr.p, lo - sink, top, b.pierWidth * 0.5f, b.pierDepth * 0.5f,
                  fr.yaw, tile);
        } else {
            // Columns spread under the deck, tied by a cap beam.
            const float capH = std::min(1.0f, std::max(b.pierWidth, 0.3f));
            const float spread = hw * 0.8f;
            for (int c = 0; c < b.pierCols; ++c) {
                const float lat = -spread + 2.0f * spread * static_cast<float>(c) /
                                                static_cast<float>(b.pierCols - 1);
                const glm::vec3 at = fr.p + fr.r * lat;
                const float lo = ground(at);
                if (top - capH - lo < 0.2f) continue;
                block(sPier, at, lo - sink, top - capH, b.pierWidth * 0.5f, b.pierWidth * 0.5f,
                      fr.yaw, tile);
            }
            block(sPier, fr.p, top - capH, top, b.pierWidth * 0.6f, spread + b.pierWidth * 0.6f,
                  fr.yaw, tile);
        }
        if (!piece()) break;
    }

    // --- Arches ---------------------------------------------------------------
    if (b.arch == bridgegen::Arch::Masonry) {
        // Every span between supports gets an arch; the fill between its ring and
        // the deck is one solid, its underside the arch itself.
        std::vector<float> sup{std::min(b.abutment, len * 0.5f)};
        for (float st : piers) sup.push_back(st);
        sup.push_back(len - std::min(b.abutment, len * 0.5f));
        const float halfP = b.pierWidth * 0.5f;
        for (std::size_t k = 0; k + 1 < sup.size(); ++k) {
            const float sa = sup[k] + (k == 0 ? 0.0f : halfP);
            const float sb = sup[k + 1] - (k + 2 == sup.size() ? 0.0f : halfP);
            const float span = sb - sa;
            if (span < 1.0f) continue;
            const float rise = std::min(b.archRise, span * 0.5f);
            const int rows = std::max(12, static_cast<int>(span / 0.7f));
            std::vector<Ring> fill;
            fill.reserve(rows + 1);
            for (int i = 0; i <= rows; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(rows);
                const Frame fr = frameAt(f, sa + span * u);
                const float x = 2.0f * u - 1.0f;
                const float topY = fr.p.y - b.thick;
                // Segmental-to-semicircular: an ellipse quadrant scaled to `rise`.
                const float bot = topY - b.archRib - rise + rise * std::sqrt(std::max(0.0f, 1.0f - x * x));
                const float hwf = hw * 0.96f;   // the slab edge reads as a cornice
                fill.push_back({fr.p + fr.r * -hwf + kUp * (bot - fr.p.y),
                                fr.p + fr.r *  hwf + kUp * (bot - fr.p.y),
                                fr.p + fr.r *  hwf + kUp * (topY - fr.p.y),
                                fr.p + fr.r * -hwf + kUp * (topY - fr.p.y)});
            }
            loft(sPier, fill, tile);
            // Without piers the ends need something to spring from.
            for (float st : {sa, sb}) {
                const bool atPier = std::any_of(piers.begin(), piers.end(),
                    [&](float p) { return std::abs(p - st) <= halfP + 0.01f; });
                if (atPier) continue;
                const Frame fr = frameAt(f, st);
                const float springY = fr.p.y - b.thick - b.archRib - rise;
                block(sPier, fr.p, ground(fr.p) - sink, springY + 0.05f, halfP, hw * 0.96f,
                      fr.yaw, tile);
            }
            if (!piece()) break;
        }
    } else if (b.arch != bridgegen::Arch::None && archB - archA > 4.0f) {
        const float span = archB - archA;
        const bool  above = b.arch == bridgegen::Arch::Above;
        // Above: ribs outside the deck edges, springing at deck level.
        // Below: ribs under the deck, springing `archRise` under the soffit.
        const float lat = above ? hw + b.archRib * 0.5f : hw * 0.55f;
        auto archY = [&](float st) {
            const float u = (st - archA) / span;
            const float bump = 4.0f * u * (1.0f - u);
            return above ? deckY(st) + b.archRise * bump
                         : deckY(st) + soffit - b.archRise * (1.0f - bump) - b.archRib * 0.5f;
        };
        const int rows = std::max(16, static_cast<int>(span / 1.5f));
        for (float side : {-1.0f, 1.0f}) {
            std::vector<glm::vec3> pts;
            for (int i = 0; i <= rows; ++i) {
                const float st = archA + span * static_cast<float>(i) / static_cast<float>(rows);
                const Frame fr = frameAt(f, st);
                glm::vec3 p = fr.p + fr.r * (side * lat);
                p.y = archY(st);
                pts.push_back(p);
            }
            curve(sSteel, pts, b.archRib, b.archRib * 1.3f, tile);
            ++res.pieces;
        }
        // Footings where an arch under the deck meets the ground.
        if (!above)
            for (float st : {archA, archB}) {
                const Frame fr = frameAt(f, st);
                const float y = archY(st);
                const float lo = std::min(ground(fr.p), y - 1.0f);
                block(sPier, fr.p, lo - sink, y + b.archRib * 0.3f, b.archRib * 1.5f,
                      lat + b.archRib * 1.2f, fr.yaw, tile);
                ++res.pieces;
            }
        // Hangers (above) or spandrel columns (below), and a brace across the
        // ribs wherever there is headroom for one.
        for (float st : evenly(archA, archB, b.hangerEvery)) {
            const Frame fr = frameAt(f, st);
            const float ya = archY(st);
            const float yd = above ? fr.p.y : fr.p.y + soffit;
            if (std::abs(ya - yd) < 0.4f) continue;
            for (float side : {-1.0f, 1.0f}) {
                glm::vec3 a = fr.p + fr.r * (side * lat), c = a;
                a.y = yd; c.y = ya;
                beam(sSteel, a, c, b.hangerThick, b.hangerThick, tile);
            }
            if (above && ya - yd > b.railHeight + 4.5f) {
                glm::vec3 l = fr.p - fr.r * lat, r = fr.p + fr.r * lat;
                l.y = r.y = ya;
                beam(sSteel, l, r, b.archRib * 0.5f, b.archRib * 0.5f, tile);
            }
            if (!above) {   // a cross-head under the deck the columns carry
                glm::vec3 l = fr.p - fr.r * (lat + b.hangerThick), r = fr.p + fr.r * (lat + b.hangerThick);
                l.y = r.y = yd - 0.3f;
                beam(sSteel, l, r, b.hangerThick, 0.6f, tile);
            }
            if (!piece()) break;
        }
    }

    // --- Truss ----------------------------------------------------------------
    if (b.trussHeight > 0.0f && len > b.trussPanel * 2.0f) {
        const int n = std::max(2, static_cast<int>(std::lround(len / b.trussPanel)));
        const float lat = hw + b.trussBar * 0.5f;
        const float bar = b.trussBar;
        auto node = [&](int k, float side, bool top) {
            const Frame fr = frameAt(f, len * static_cast<float>(k) / static_cast<float>(n));
            return fr.p + fr.r * (side * lat) + kUp * (top ? b.trussHeight : 0.0f);
        };
        for (float side : {-1.0f, 1.0f}) {
            // Chords: bottom along the whole deck, top between the end posts.
            std::vector<glm::vec3> bottom, top;
            for (int k = 0; k <= n; ++k) bottom.push_back(node(k, side, false));
            for (int k = 1; k < n; ++k) top.push_back(node(k, side, true));
            curve(sSteel, bottom, bar, bar, tile);
            curve(sSteel, top, bar, bar, tile);
            // Inclined end posts, verticals, and Pratt diagonals falling toward
            // the middle.
            beam(sSteel, node(0, side, false), node(1, side, true), bar, bar, tile);
            beam(sSteel, node(n, side, false), node(n - 1, side, true), bar, bar, tile);
            for (int k = 1; k < n; ++k) {
                beam(sSteel, node(k, side, false), node(k, side, true), bar * 0.8f, bar * 0.8f, tile);
                if (k + 1 < n) {
                    if (2 * k < n) beam(sSteel, node(k, side, true), node(k + 1, side, false),
                                        bar * 0.7f, bar * 0.7f, tile);
                    else           beam(sSteel, node(k, side, false), node(k + 1, side, true),
                                        bar * 0.7f, bar * 0.7f, tile);
                }
                if (!piece()) break;
            }
        }
        if (b.trussTop)
            for (int k = 1; k < n; ++k)
                beam(sSteel, node(k, -1.0f, true), node(k, 1.0f, true), bar * 0.7f, bar * 0.7f, tile);
    }

    // --- Towers and cables ----------------------------------------------------
    for (float st : towers) {
        const Frame fr = frameAt(f, st);
        const float topY = fr.p.y + b.towerHeight;
        const float tw = b.towerWidth;
        for (float side : {-1.0f, 1.0f}) {
            const glm::vec3 at = fr.p + fr.r * (side * legLat);
            block(sPier, at, ground(at) - sink, topY, tw * 0.65f, tw * 0.5f, fr.yaw, tile);
        }
        // Cross beams: under the deck, and at the top.
        for (float y : {fr.p.y + soffit - tw * 0.4f, topY - tw * 0.6f}) {
            glm::vec3 l = fr.p - fr.r * legLat, r = fr.p + fr.r * legLat;
            l.y = r.y = y;
            beam(sPier, l, r, tw * 1.1f, tw * 0.8f, tile);
        }
        res.pieces += 2;
    }

    if (b.cable == bridgegen::Cable::Suspension && !towers.empty()) {
        // The cable over [0, len]: parabolas between towers, gently sagging
        // backstays from the deck ends up to the first and last tower.
        std::vector<float> nodes{0.0f};
        for (float t : towers) nodes.push_back(t);
        nodes.push_back(len);
        auto cableY = [&](float st) {
            std::size_t k = 0;
            while (k + 2 < nodes.size() && st > nodes[k + 1]) ++k;
            const float a = nodes[k], c = nodes[k + 1];
            const float u = glm::clamp((st - a) / std::max(c - a, 1e-3f), 0.0f, 1.0f);
            const bool endA = (k == 0), endB = (k + 2 == nodes.size());
            const float ya = endA ? deckY(a) + 0.8f : deckY(a) + b.towerHeight - b.towerWidth * 0.6f;
            const float yc = endB ? deckY(c) + 0.8f : deckY(c) + b.towerHeight - b.towerWidth * 0.6f;
            if (endA || endB) {
                // A backstay: nearly straight, with a little sag.
                return glm::mix(ya, yc, u) - (c - a) * 0.04f * 4.0f * u * (1.0f - u);
            }
            const float low = deckY(glm::mix(a, c, 0.5f)) + b.cableSag;
            const float x = 2.0f * u - 1.0f;
            return glm::mix(low, glm::mix(ya, yc, u), x * x);
        };
        const int rows = std::max(24, static_cast<int>(len / 2.0f));
        for (float side : {-1.0f, 1.0f}) {
            std::vector<glm::vec3> pts;
            for (int i = 0; i <= rows; ++i) {
                const float st = len * static_cast<float>(i) / static_cast<float>(rows);
                const Frame fr = frameAt(f, st);
                glm::vec3 p = fr.p + fr.r * (side * legLat);
                p.y = cableY(st);
                pts.push_back(p);
            }
            curve(sSteel, pts, b.cableThick, b.cableThick, tile);
        }
        // Hangers only on the main spans -- a backstay carries nothing.
        for (std::size_t k = 1; k + 2 < nodes.size(); ++k)
            for (float st : evenly(nodes[k], nodes[k + 1], b.hangerEvery)) {
                const Frame fr = frameAt(f, st);
                const float y = cableY(st);
                if (y - fr.p.y < 0.6f) continue;
                for (float side : {-1.0f, 1.0f}) {
                    glm::vec3 a = fr.p + fr.r * (side * legLat), c = a;
                    c.y = y;
                    beam(sSteel, a, c, b.hangerThick, b.hangerThick, tile);
                }
                if (!piece()) break;
            }
    } else if (b.cable == bridgegen::Cable::Stayed) {
        // Stays fan out either side of each tower to half way to the next one
        // (or to the deck end). The nearest stay anchors lowest on the tower.
        for (std::size_t ti = 0; ti < towers.size(); ++ti) {
            const float t = towers[ti];
            const float reachL = (ti == 0) ? t - 2.0f : (t - towers[ti - 1]) * 0.5f - 1.0f;
            const float reachR = (ti + 1 == towers.size()) ? len - t - 2.0f
                                                           : (towers[ti + 1] - t) * 0.5f - 1.0f;
            const Frame tf = frameAt(f, t);
            const float topY = tf.p.y + b.towerHeight - b.towerWidth * 0.8f;
            for (float dir : {-1.0f, 1.0f}) {
                const float reach = dir < 0.0f ? reachL : reachR;
                const int n = static_cast<int>(reach / b.stayEvery);
                if (n < 1) continue;
                const float step = std::min(0.9f, b.towerHeight * 0.4f / static_cast<float>(n));
                for (int i = 1; i <= n; ++i) {
                    const float st = t + dir * b.stayEvery * static_cast<float>(i);
                    const Frame fr = frameAt(f, st);
                    for (float side : {-1.0f, 1.0f}) {
                        glm::vec3 deckPt = fr.p + fr.r * (side * legLat);
                        glm::vec3 towPt  = tf.p + tf.r * (side * legLat);
                        towPt.y = topY - step * static_cast<float>(n - i);
                        beam(sSteel, deckPt, towPt, b.cableThick, b.cableThick, tile);
                    }
                    if (!piece()) break;
                }
            }
        }
    }

    // --- Batches ----------------------------------------------------------------
    const AssetId mat[3] = {sIn.matA.valid() ? sIn.matA : pal.primary,
                            sIn.matB.valid() ? sIn.matB : pal.secondary,
                            sIn.matC.valid() ? sIn.matC : pal.tertiary};
    for (int i = 0; i < 3; ++i) {
        if (slot[i].empty()) continue;
        Batch bt;
        bt.material = mat[i];
        bt.lo       = slot[i].lo;
        bt.hi       = slot[i].hi;
        bt.data     = std::move(slot[i].data);
        res.verts += static_cast<int>(bt.data.vertices.size());
        res.batches.push_back(std::move(bt));
    }
    res.budgetHit = budget <= 0;

    // --- Collision ----------------------------------------------------------------
    // The deck in ~4 m boxes that climb with it (a car drives on these), the
    // edges as low walls so it cannot drive off, and every upright as a box.
    // Arches, trusses and cables are left out: nothing drives on those.
    if (sIn.collide) {
        const float edgeH = b.rail == bridgegen::Rail::None ? 0.0f : b.railHeight;
        std::size_t i = 0;
        while (i < last) {
            std::size_t j = i + 1;
            while (j < last && f[j].station - f[i].station < 4.0f) ++j;
            const glm::vec3 a = f[i].p, c = f[j].p;
            const glm::vec3 d(c.x - a.x, 0.0f, c.z - a.z);
            const float plan = glm::length(d);
            if (plan > 1e-4f) {
                const float yaw   = glm::degrees(std::atan2(d.x, d.z));
                const float pitch = glm::degrees(std::atan2(c.y - a.y, plan));
                const float half  = glm::length(c - a) * 0.5f;
                const glm::vec3 mid = (a + c) * 0.5f;
                // Up is the deck's own up (tilted with the pitch), not world up.
                const glm::vec3 fwd = glm::normalize(c - a);
                const glm::vec3 side = glm::normalize(glm::cross(kUp, fwd));
                const glm::vec3 up = glm::cross(fwd, side);
                Collider col;
                col.yaw = yaw; col.pitch = pitch;
                col.center = mid - up * (b.thick * 0.5f);
                col.half   = glm::vec3(hw, b.thick * 0.5f, half);
                res.colliders.push_back(col);
                if (edgeH > 0.0f)
                    for (float s : {-1.0f, 1.0f}) {
                        Collider e = col;
                        e.center = mid + side * (s * railLat) + up * (edgeH * 0.5f);
                        e.half   = glm::vec3(b.railThick * 0.5f, edgeH * 0.5f, half);
                        res.colliders.push_back(e);
                    }
            }
            i = j;
        }
        auto upright = [&](float st, float lat, float halfAlong, float halfAcross, float top) {
            const Frame fr = frameAt(f, st);
            const glm::vec3 at = fr.p + fr.r * lat;
            const float lo = ground(at) - sink;
            if (top - lo < 0.2f) return;
            Collider col;
            col.center = glm::vec3(at.x, (lo + top) * 0.5f, at.z);
            col.half   = glm::vec3(halfAcross, (top - lo) * 0.5f, halfAlong);
            col.yaw    = glm::degrees(fr.yaw);
            res.colliders.push_back(col);
        };
        for (float st : piers)
            upright(st, 0.0f, b.pierWidth * 0.5f,
                    b.pierCols <= 1 ? b.pierDepth * 0.5f : hw * 0.8f + b.pierWidth * 0.5f,
                    deckY(st) + soffit);
        for (float st : towers)
            for (float s : {-1.0f, 1.0f})
                upright(st, s * legLat, b.towerWidth * 0.65f, b.towerWidth * 0.5f,
                        deckY(st) + b.towerHeight);
    }
    return res;
}

} // namespace splinegen

// --- Persistence ----------------------------------------------------------------

namespace bridgegen {

bool Style::operator==(const Style& o) const {
    return width == o.width && thick == o.thick && girder == o.girder &&
           camber == o.camber && rail == o.rail && railHeight == o.railHeight &&
           railThick == o.railThick && postEvery == o.postEvery &&
           pierEvery == o.pierEvery && pierCols == o.pierCols &&
           pierWidth == o.pierWidth && pierDepth == o.pierDepth &&
           abutment == o.abutment && arch == o.arch && archRise == o.archRise &&
           archRib == o.archRib && archInset == o.archInset &&
           hangerEvery == o.hangerEvery && hangerThick == o.hangerThick &&
           trussHeight == o.trussHeight && trussPanel == o.trussPanel &&
           trussBar == o.trussBar && trussTop == o.trussTop && cable == o.cable &&
           towerHeight == o.towerHeight && towerAt == o.towerAt &&
           towerWidth == o.towerWidth && cableSag == o.cableSag &&
           cableThick == o.cableThick && stayEvery == o.stayEvery;
}

void save(nlohmann::json& j, const Style& s) {
    j["width"]       = s.width;
    j["thick"]       = s.thick;
    j["girder"]      = s.girder;
    j["camber"]      = s.camber;
    j["rail"]        = static_cast<int>(s.rail);
    j["railHeight"]  = s.railHeight;
    j["railThick"]   = s.railThick;
    j["postEvery"]   = s.postEvery;
    j["pierEvery"]   = s.pierEvery;
    j["pierCols"]    = s.pierCols;
    j["pierWidth"]   = s.pierWidth;
    j["pierDepth"]   = s.pierDepth;
    j["abutment"]    = s.abutment;
    j["arch"]        = static_cast<int>(s.arch);
    j["archRise"]    = s.archRise;
    j["archRib"]     = s.archRib;
    j["archInset"]   = s.archInset;
    j["hangerEvery"] = s.hangerEvery;
    j["hangerThick"] = s.hangerThick;
    j["trussHeight"] = s.trussHeight;
    j["trussPanel"]  = s.trussPanel;
    j["trussBar"]    = s.trussBar;
    j["trussTop"]    = s.trussTop;
    j["cable"]       = static_cast<int>(s.cable);
    j["towerHeight"] = s.towerHeight;
    j["towerAt"]     = s.towerAt;
    j["towerWidth"]  = s.towerWidth;
    j["cableSag"]    = s.cableSag;
    j["cableThick"]  = s.cableThick;
    j["stayEvery"]   = s.stayEvery;
}

void load(const nlohmann::json& j, Style& s) {
    auto en = [&](const char* key, auto& out, int count) {
        const int v = j.value(key, static_cast<int>(out));
        out = static_cast<std::remove_reference_t<decltype(out)>>(glm::clamp(v, 0, count - 1));
    };
    s.width       = j.value("width", s.width);
    s.thick       = j.value("thick", s.thick);
    s.girder      = j.value("girder", s.girder);
    s.camber      = j.value("camber", s.camber);
    en("rail", s.rail, static_cast<int>(Rail::Count));
    s.railHeight  = j.value("railHeight", s.railHeight);
    s.railThick   = j.value("railThick", s.railThick);
    s.postEvery   = j.value("postEvery", s.postEvery);
    s.pierEvery   = j.value("pierEvery", s.pierEvery);
    s.pierCols    = j.value("pierCols", s.pierCols);
    s.pierWidth   = j.value("pierWidth", s.pierWidth);
    s.pierDepth   = j.value("pierDepth", s.pierDepth);
    s.abutment    = j.value("abutment", s.abutment);
    en("arch", s.arch, static_cast<int>(Arch::Count));
    s.archRise    = j.value("archRise", s.archRise);
    s.archRib     = j.value("archRib", s.archRib);
    s.archInset   = j.value("archInset", s.archInset);
    s.hangerEvery = j.value("hangerEvery", s.hangerEvery);
    s.hangerThick = j.value("hangerThick", s.hangerThick);
    s.trussHeight = j.value("trussHeight", s.trussHeight);
    s.trussPanel  = j.value("trussPanel", s.trussPanel);
    s.trussBar    = j.value("trussBar", s.trussBar);
    s.trussTop    = j.value("trussTop", s.trussTop);
    en("cable", s.cable, static_cast<int>(Cable::Count));
    s.towerHeight = j.value("towerHeight", s.towerHeight);
    s.towerAt     = j.value("towerAt", s.towerAt);
    s.towerWidth  = j.value("towerWidth", s.towerWidth);
    s.cableSag    = j.value("cableSag", s.cableSag);
    s.cableThick  = j.value("cableThick", s.cableThick);
    s.stayEvery   = j.value("stayEvery", s.stayEvery);
}

} // namespace bridgegen
