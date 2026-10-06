// A tram line's track, the way a street tramway is built: in the road, rails
// flush with a band of setts and a groove beside each head for the flange; on
// its own ground, light track on ties. Two tracks (one per direction) that meet
// on one at an open end, the overhead wire on masts at the kerb, and a sign at
// every stop. The layout -- where each track runs, where the line stops -- is
// TramTrack.hpp's, so the trams (TramSim) run exactly on what is drawn here.
#include <algorithm>
#include <cmath>

#include "SplineGenDetail.hpp"
#include "TramTrack.hpp"

namespace splinegen::detail {

namespace {

// The centre frames moved sideways onto track `k` (Frame::r is the LEFT, so
// "right" is -r). Station, tangent and yaw stay the centreline's: the sleepers
// and the wire keep the centre's phasing, which is what makes a chunk boundary
// invisible on the two tracks alike.
std::vector<Frame> trackFrames(const std::vector<Frame>& f, std::size_t i0, std::size_t i1,
                               const Style& s, float len, bool closed, int k) {
    std::vector<Frame> out(f.begin() + static_cast<std::ptrdiff_t>(i0),
                           f.begin() + static_cast<std::ptrdiff_t>(i1) + 1);
    for (Frame& fr : out)
        fr.p -= fr.r * tramtrack::offset(fr.station, len, s.tracks, s.trackSpacing, closed, k);
    return out;
}

// The centre frame at station `st` (interpolated), for things placed by station.
Frame frameAt(const std::vector<Frame>& f, float st) {
    std::size_t i = 0;
    while (i + 2 < f.size() && f[i + 1].station < st) ++i;
    const Frame& a = f[i];
    const Frame& b = f[std::min(i + 1, f.size() - 1)];
    const float span = std::max(b.station - a.station, 1e-5f);
    const float u = std::clamp((st - a.station) / span, 0.0f, 1.0f);
    Frame o = a;
    o.p = glm::mix(a.p, b.p, u);
    o.station = st;
    return o;
}

} // namespace

void tramChunk(Slot slot[4], const std::vector<Frame>& f, std::size_t i0, std::size_t i1,
               const Style& s, const std::vector<char>& onRoad, bool closed, int& budget,
               int& pieces, bool capStart, bool capEnd) {
    if (i1 <= i0 || f.size() < 2) return;
    const float len   = f.back().station;
    const int   tracks = std::clamp(s.tracks, 1, 2);
    const float g     = s.gauge * 0.5f;
    const float hw    = s.railWidth * 0.5f;
    const float tile  = s.texTile;
    const float offTop = tramtrack::railTopOffRoad(s.sleeperHeight, s.railHeight);
    auto road = [&](std::size_t i) { return i < onRoad.size() && onRoad[i] != 0; };

    Profile lifted = railProfile(s.railWidth, s.railHeight);
    for (glm::vec2& q : lifted) q.y += s.sleeperHeight;

    for (int k = 0; k < tracks; ++k) {
        const std::vector<Frame> tf = trackFrames(f, i0, i1, s, len, closed, k);
        const std::size_t n = tf.size();
        // Runs of samples that are all in a road, or all off one. Neighbouring
        // runs share their boundary sample, so the track is continuous across.
        std::size_t a = 0;
        while (a + 1 < n) {
            const bool inRoad = road(i0 + a);
            std::size_t b = a + 1;
            while (b + 1 < n && road(i0 + b) == inRoad) ++b;
            const bool ca = capStart && a == 0, cb = capEnd && b == n - 1;
            if (inRoad) {
                // The paving band. Its bottom reaches below the asphalt, so its
                // edges never show daylight where the street's surface dips.
                sweep(slot[2], tf, a, b,
                      rectProfile(g + tramtrack::kBandMargin, tramtrack::kBandTop - 0.05f,
                                  tramtrack::kBandTop),
                      0.0f, tile, ca, cb);
                // The heads, flush but a hair proud so they catch the light...
                const Profile head = rectProfile(hw, tramtrack::kBandTop - 0.01f, tramtrack::kRailTop);
                sweep(slot[0], tf, a, b, head, -g, tile, ca, cb);
                sweep(slot[0], tf, a, b, head,  g, tile, ca, cb);
                // ...and the groove inside each, dark, for the flange.
                const float gw = 0.022f;
                const Profile groove = rectProfile(gw, tramtrack::kBandTop - 0.01f,
                                                   tramtrack::kBandTop + 0.003f);
                sweep(slot[1], tf, a, b, groove, -(g - hw - gw), tile, ca, cb);
                sweep(slot[1], tf, a, b, groove,  (g - hw - gw), tile, ca, cb);
            } else {
                // Its own ground: ties and Vignoles rail, no ballast bed.
                const std::vector<Stop> ties = stopsIn(tf, a, b, s.sleeperSpacing, budget);
                for (const Stop& st : ties)
                    appendBox(slot[1], st.p + glm::vec3(0.0f, s.sleeperHeight * 0.5f, 0.0f),
                              glm::vec3(s.sleeperLength * 0.5f, s.sleeperHeight * 0.5f,
                                        s.sleeperWidth * 0.5f),
                              st.yaw, tile);
                pieces += static_cast<int>(ties.size());
                sweep(slot[0], tf, a, b, lifted, -g, tile, ca, cb);
                sweep(slot[0], tf, a, b, lifted,  g, tile, ca, cb);
            }
            // The contact wire over this track, at a fixed height above ITS rail
            // head -- so the pantograph meets it in the street and off it alike.
            if (s.catenary) {
                const float top = (inRoad ? tramtrack::kRailTop : offTop) + 5.62f;
                sweep(slot[0], tf, a, b, rectProfile(0.012f, top - 0.012f, top + 0.012f),
                      0.0f, tile, false, false);
            }
            a = b;
        }
    }

    // Masts at the kerb, every 30 m, with an arm out over the track. Placed by
    // centre station like everything that repeats, so the two tracks' masts
    // stand opposite each other.
    if (s.catenary) {
        const float every = 30.0f;
        for (const Stop& st : stopsIn(f, i0, i1, every, budget)) {
            const float station = st.index * every;
            const Frame fr = frameAt(f, station);
            std::size_t j = i0;
            while (j < i1 && f[j].station < station) ++j;
            const float railTop = road(j) ? tramtrack::kRailTop : offTop;
            for (int k = 0; k < tracks; ++k) {
                const float o = tramtrack::offset(station, len, s.tracks, s.trackSpacing, closed, k);
                // Outside the track, on its right for track 0 and left for track 1.
                const float side = (k == 0) ? 1.0f : -1.0f;
                const float mast = o + side * (g + 1.75f);
                const glm::vec3 base = fr.p - fr.r * mast;
                const float h = 6.4f;
                appendBox(slot[0], base + glm::vec3(0.0f, h * 0.5f - 0.3f, 0.0f),
                          glm::vec3(0.09f, h * 0.5f, 0.09f), fr.yaw, tile);
                // The arm: from the mast to over the track, just above the wire.
                const float armY = railTop + 5.95f;
                const float mid = (mast + o) * 0.5f;
                appendBox(slot[0], fr.p - fr.r * mid + glm::vec3(0.0f, armY, 0.0f),
                          glm::vec3(std::abs(mast - o) * 0.5f, 0.035f, 0.035f), fr.yaw, tile);
                // ...and the hanger down to the wire.
                appendBox(slot[0], fr.p - fr.r * o + glm::vec3(0.0f, railTop + 5.78f, 0.0f),
                          glm::vec3(0.015f, 0.17f, 0.015f), fr.yaw, tile);
                pieces += 3;
                if (tracks == 1) break;
            }
        }
    }

    // A sign at each stop, at the kerb on the right of whoever stops there:
    // a pole and the yellow plate. (The trams stop with their front at it.)
    const float from = f[i0].station, to = f[i1].station;
    for (float st : tramtrack::stops(len, s.stopEvery, closed)) {
        if (st < from || st >= to) continue;
        if (budget <= 0) break;
        const Frame fr = frameAt(f, st);
        for (int k = 0; k < tracks; ++k) {
            const float o = tramtrack::offset(st, len, s.tracks, s.trackSpacing, closed, k);
            const float side = (k == 0) ? 1.0f : -1.0f;
            const float at = o + side * (g + 1.45f);
            const glm::vec3 base = fr.p - fr.r * at;
            appendBox(slot[0], base + glm::vec3(0.0f, 1.3f, 0.0f),
                      glm::vec3(0.035f, 1.3f, 0.035f), fr.yaw, tile);
            appendBox(slot[3], base + glm::vec3(0.0f, 2.35f, 0.0f),
                      glm::vec3(0.30f, 0.30f, 0.025f), fr.yaw, tile);
            pieces += 2;
            --budget;
            if (tracks == 1) {
                // One track, both directions: a sign on each side of it.
                const glm::vec3 other = fr.p + fr.r * at;
                appendBox(slot[0], other + glm::vec3(0.0f, 1.3f, 0.0f),
                          glm::vec3(0.035f, 1.3f, 0.035f), fr.yaw, tile);
                appendBox(slot[3], other + glm::vec3(0.0f, 2.35f, 0.0f),
                          glm::vec3(0.30f, 0.30f, 0.025f), fr.yaw, tile);
                pieces += 2;
            }
        }
    }
}

} // namespace splinegen::detail
