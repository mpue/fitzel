#include "RiverGen.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/gtc/constants.hpp>

namespace rivergen {
namespace {

float smooth01(float e) { return e * e * (3.0f - 2.0f * e); }

// A cheap deterministic hash -> [0,1), for the per-stretch jitter. Same shape as
// the one splinegen uses, and for the same reason: nudging one control point
// must not reshuffle a river the author has been looking at.
float hash1(unsigned n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n = n ^ (n >> 4);
    n *= 0x27d4eb2du;
    n = n ^ (n >> 15);
    return static_cast<float>(n & 0xffffffu) / 16777216.0f;
}

// How much of the channel's depth the section has at `u` = |offset| / half-width.
// 1 in the deepest part, 0 at the waterline. The one place the three kinds
// actually differ -- sectionHeight and the surface strip both come through here,
// so the bed the water is drawn over is the bed that was cut.
float depthFactor(Kind k, const Style& st, float u) {
    const float bf = glm::clamp(st.bedFlat, 0.0f, 0.95f);
    u = glm::clamp(u, 0.0f, 1.0f);

    if (u <= bf) return 1.0f;
    const float e = (u - bf) / (1.0f - bf);
    switch (k) {
        // Worn bowl: most of the depth is lost close in, so the margins run
        // shallow over gravel -- which is what a brook is.
        case Kind::Brook: return (1.0f - e) * (1.0f - e);
        // A trough. With bedFlat high (where the canal presets put it) this is a
        // wall; with it low it is a graded ditch.
        case Kind::Canal: return 1.0f - e;
        // Flat bed, banks meeting the waterline tangentially.
        default:          return 1.0f - smooth01(e);
    }
}

// The meander wave, -1..1, as a function of distance along the course. Three
// sines at incommensurate wavelengths rather than one: a single sine reads as a
// sine at any amplitude, and the whole point is that it should not.
float wander(float s, float wavelength, unsigned seed) {
    const float k = 6.2831853f / std::max(wavelength, 1.0f);
    const float p0 = hash1(seed * 7919u + 1u) * 6.2831853f;
    const float p1 = hash1(seed * 7919u + 2u) * 6.2831853f;
    const float p2 = hash1(seed * 7919u + 3u) * 6.2831853f;
    const float w = 0.72f * std::sin(k * s + p0) +
                    0.34f * std::sin(k * s * 0.61f + p1) +
                    0.16f * std::sin(k * s * 1.73f + p2);
    return glm::clamp(w / 0.95f, -1.0f, 1.0f);
}

} // namespace

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Brook: return "Brook";
        case Kind::River: return "River";
        case Kind::Canal: return "Canal";
        default:          return "?";
    }
}

bool Style::operator==(const Style& o) const {
    return width == o.width && widen == o.widen && depth == o.depth &&
           bedFlat == o.bedFlat && bankWidth == o.bankWidth &&
           bankRise == o.bankRise && minSlope == o.minSlope &&
           maxCut == o.maxCut && smooth == o.smooth && autoFlow == o.autoFlow &&
           flip == o.flip && rapidSlope == o.rapidSlope &&
           fallSlope == o.fallSlope && fallMin == o.fallMin &&
           poolLength == o.poolLength && plunge == o.plunge &&
           shallow == o.shallow && deep == o.deep && flowSpeed == o.flowSpeed &&
           clarity == o.clarity && reflect == o.reflect &&
           rippleScale == o.rippleScale && ripple == o.ripple &&
           foamWidth == o.foamWidth && sparkle == o.sparkle &&
           bankLayer == o.bankLayer && bankPaint == o.bankPaint &&
           bankBlend == o.bankBlend && stones == o.stones &&
           stoneSize == o.stoneSize && stoneSpread == o.stoneSpread &&
           reeds == o.reeds && reedHeight == o.reedHeight &&
           reedDepth == o.reedDepth && stoneColor == o.stoneColor &&
           reedColor == o.reedColor && meander == o.meander &&
           meanderLength == o.meanderLength && widthVary == o.widthVary &&
           depthVary == o.depthVary && bendScour == o.bendScour &&
           bendEase == o.bendEase &&
           current == o.current && seed == o.seed;
}

// --- Presets -----------------------------------------------------------------

const char* presetName(Preset p) {
    switch (p) {
        case Preset::Brook:          return "Brook";
        case Preset::MountainStream: return "Mountain stream";
        case Preset::Creek:          return "Creek";
        case Preset::River:          return "River";
        case Preset::WideRiver:      return "Wide river";
        case Preset::Ditch:          return "Ditch";
        case Preset::Canal:          return "Canal";
        case Preset::MillRace:       return "Mill race";
        default:                     return "?";
    }
}

Kind presetKind(Preset p) {
    switch (p) {
        case Preset::Brook:
        case Preset::MountainStream:
        case Preset::Creek:     return Kind::Brook;
        case Preset::River:
        case Preset::WideRiver: return Kind::River;
        default:                return Kind::Canal;
    }
}

Style preset(Preset p) {
    Style s;
    switch (p) {
        case Preset::Brook:
            s.width = 2.0f;  s.widen = 1.15f; s.depth = 0.35f; s.bedFlat = 0.20f;
            s.bankWidth = 1.6f; s.bankRise = 0.25f;
            s.minSlope = 0.006f; s.rapidSlope = 0.040f; s.fallSlope = 0.35f;
            s.fallMin = 0.6f; s.poolLength = 4.0f; s.plunge = 0.4f;
            s.shallow = {0.34f, 0.50f, 0.45f}; s.deep = {0.06f, 0.20f, 0.21f};
            s.flowSpeed = 1.1f; s.clarity = 2.4f; s.ripple = 0.055f;
            s.rippleScale = 1.1f; s.foamWidth = 0.20f; s.bankPaint = 1.2f; s.stones = 26.0f; s.stoneSize = 0.22f; s.reeds = 5.0f; s.reedHeight = 0.7f;
            s.meander = 3.0f; s.meanderLength = 26.0f; s.bendEase = 0.8f; s.widthVary = 0.30f; s.depthVary = 0.35f; s.bendScour = 0.35f;
            s.current = 1.0f;
            break;
        case Preset::MountainStream:
            s.width = 3.2f;  s.widen = 1.20f; s.depth = 0.50f; s.bedFlat = 0.15f;
            s.bankWidth = 2.2f; s.bankRise = 0.50f;
            s.minSlope = 0.020f; s.rapidSlope = 0.030f; s.fallSlope = 0.25f;
            s.fallMin = 0.8f; s.poolLength = 5.0f; s.plunge = 1.0f; s.smooth = 5;
            s.shallow = {0.38f, 0.56f, 0.56f}; s.deep = {0.05f, 0.22f, 0.27f};
            s.flowSpeed = 2.0f; s.clarity = 2.8f; s.ripple = 0.090f;
            s.rippleScale = 1.4f; s.foamWidth = 0.30f; s.bankPaint = 1.8f; s.stones = 55.0f; s.stoneSize = 0.40f; s.stoneSpread = 2.2f; s.reeds = 0.0f;
            s.meander = 1.2f; s.meanderLength = 40.0f; s.bendEase = 0.7f; s.widthVary = 0.25f; s.depthVary = 0.30f; s.bendScour = 0.20f;
            s.sparkle = 0.9f; s.current = 3.0f;
            break;
        case Preset::Creek:
            s.width = 5.0f;  s.widen = 1.30f; s.depth = 0.80f; s.bedFlat = 0.30f;
            s.bankWidth = 3.5f; s.bankRise = 0.35f;
            s.minSlope = 0.004f; s.rapidSlope = 0.035f; s.fallSlope = 0.35f;
            s.fallMin = 0.8f; s.poolLength = 6.0f; s.plunge = 0.6f;
            s.shallow = {0.30f, 0.48f, 0.46f}; s.deep = {0.04f, 0.15f, 0.19f};
            s.flowSpeed = 0.9f; s.clarity = 1.8f; s.foamWidth = 0.30f; s.bankPaint = 2.4f; s.stones = 16.0f; s.stoneSize = 0.30f; s.reeds = 9.0f;
            s.meander = 7.0f; s.meanderLength = 60.0f; s.bendEase = 0.9f; s.widthVary = 0.28f; s.depthVary = 0.35f; s.bendScour = 0.45f;
            s.current = 1.2f;
            break;
        case Preset::River:
            s.width = 14.0f; s.widen = 1.50f; s.depth = 2.20f; s.bedFlat = 0.50f;
            s.bankWidth = 10.0f; s.bankRise = 0.60f;
            s.minSlope = 0.0015f; s.rapidSlope = 0.020f; s.fallSlope = 0.30f;
            s.fallMin = 1.5f; s.poolLength = 14.0f; s.plunge = 1.2f; s.smooth = 14;
            s.shallow = {0.24f, 0.40f, 0.42f}; s.deep = {0.02f, 0.09f, 0.14f};
            s.flowSpeed = 0.55f; s.clarity = 0.9f; s.ripple = 0.035f;
            s.rippleScale = 0.5f; s.foamWidth = 0.55f; s.bankPaint = 4.5f; s.stones = 6.0f; s.stoneSize = 0.55f; s.stoneSpread = 3.0f; s.reeds = 12.0f; s.reedHeight = 1.4f; s.reedDepth = 0.6f;
            s.meander = 26.0f; s.meanderLength = 170.0f; s.bendEase = 1.1f; s.widthVary = 0.25f; s.depthVary = 0.40f; s.bendScour = 0.55f;
            s.current = 1.4f;
            break;
        case Preset::WideRiver:
            s.width = 34.0f; s.widen = 1.35f; s.depth = 4.00f; s.bedFlat = 0.60f;
            s.bankWidth = 18.0f; s.bankRise = 0.80f;
            s.minSlope = 0.0008f; s.rapidSlope = 0.015f; s.fallSlope = 0.30f;
            s.fallMin = 2.5f; s.poolLength = 24.0f; s.plunge = 1.5f; s.smooth = 20;
            s.shallow = {0.26f, 0.42f, 0.45f}; s.deep = {0.01f, 0.07f, 0.12f};
            s.flowSpeed = 0.40f; s.clarity = 0.6f; s.ripple = 0.030f;
            s.rippleScale = 0.35f; s.foamWidth = 0.90f; s.bankPaint = 9.0f; s.stones = 3.0f; s.stoneSize = 0.8f; s.stoneSpread = 5.0f; s.reeds = 10.0f; s.reedHeight = 1.8f; s.reedDepth = 0.9f;
            s.meander = 55.0f; s.meanderLength = 380.0f; s.bendEase = 1.2f; s.widthVary = 0.22f; s.depthVary = 0.40f; s.bendScour = 0.60f;
            s.current = 1.1f;
            break;
        case Preset::Ditch:
            s.width = 1.6f;  s.widen = 1.0f;  s.depth = 0.50f; s.bedFlat = 0.60f;
            s.bankWidth = 1.5f; s.bankRise = 0.35f;
            s.minSlope = 0.002f; s.rapidSlope = 0.060f; s.fallSlope = 0.50f;
            s.fallMin = 0.5f; s.poolLength = 3.0f; s.plunge = 0.2f;
            s.shallow = {0.34f, 0.40f, 0.30f}; s.deep = {0.10f, 0.14f, 0.09f};
            s.flowSpeed = 0.35f; s.clarity = 1.2f; s.ripple = 0.030f;
            s.foamWidth = 0.15f; s.sparkle = 0.3f; s.bankPaint = 0.5f; s.reeds = 14.0f; s.reedHeight = 0.8f; s.reedDepth = 0.4f;
            s.meander = 0.0f; s.widthVary = 0.0f; s.depthVary = 0.0f; s.bendEase = 0.0f;
            s.current = 0.4f;
            break;
        case Preset::Canal:
            s.width = 8.0f;  s.widen = 1.0f;  s.depth = 2.00f; s.bedFlat = 0.88f;
            s.bankWidth = 3.0f; s.bankRise = 0.50f;
            s.minSlope = 0.0f; s.smooth = 30;
            s.rapidSlope = 0.50f; s.fallSlope = 0.90f; s.fallMin = 2.0f;
            s.poolLength = 10.0f; s.plunge = 0.3f;
            s.shallow = {0.24f, 0.34f, 0.34f}; s.deep = {0.03f, 0.09f, 0.12f};
            s.flowSpeed = 0.20f; s.clarity = 0.7f; s.ripple = 0.022f;
            s.rippleScale = 0.45f; s.foamWidth = 0.20f; s.bankPaint = 0.8f;
            s.bendEase = 0.0f;
            s.sparkle = 0.4f; s.current = 0.2f;
            break;
        case Preset::MillRace:
            s.width = 2.5f;  s.widen = 1.0f;  s.depth = 0.80f; s.bedFlat = 0.80f;
            s.bankWidth = 1.5f; s.bankRise = 0.40f;
            s.minSlope = 0.010f; s.smooth = 12;
            s.rapidSlope = 0.030f; s.fallSlope = 0.35f; s.fallMin = 0.8f;
            s.poolLength = 4.0f; s.plunge = 0.5f;
            s.shallow = {0.36f, 0.52f, 0.50f}; s.deep = {0.05f, 0.15f, 0.18f};
            s.flowSpeed = 2.2f; s.clarity = 1.6f; s.ripple = 0.060f;
            s.rippleScale = 1.2f; s.foamWidth = 0.25f; s.bankPaint = 0.8f;
            s.bendEase = 0.0f;
            s.current = 3.5f;
            break;
        default: break;
    }
    return s;
}

Style preset(Kind k) {
    switch (k) {
        case Kind::River: return preset(Preset::River);
        case Kind::Canal: return preset(Preset::Canal);
        default:          return preset(Preset::Brook);
    }
}

// --- The section -------------------------------------------------------------

float sectionHeight(Kind k, const Style& st, float d, float half, float shift,
                    float surf, float bed, float g) {
    const float hw = std::max(half, 0.05f);
    // The deep line, clamped well inside the waterline: a section whose deepest
    // point sat ON the bank would have a vertical face there and no bed at all.
    const float sh = glm::clamp(shift, -hw * 0.85f, hw * 0.85f);
    const float ad = std::fabs(d);
    if (ad >= hw) {
        // The bank: up over the lip and then back to whatever the ground was
        // doing. Blending to `g` rather than stopping at the lip is what keeps a
        // channel from being a slot with two walls -- and it means a river in a
        // valley leaves the valley alone, because there the natural ground is
        // already where the blend is heading.
        const float t   = glm::clamp((ad - hw) / std::max(st.bankWidth, 0.05f),
                                     0.0f, 1.0f);
        const float lip = surf + st.bankRise * std::min(t / 0.30f, 1.0f);
        return glm::mix(lip, g, smooth01(t));
    }
    // Where the deep line has moved, the section is measured from IT rather than
    // from the centreline -- so the outer side is compressed into the short
    // distance left to that bank (steep, scoured) and the inner side is stretched
    // over the long one (a point bar running out under the water). Both sides
    // still reach the waterline exactly at +/- half, which is what keeps the cut
    // meeting the ground where the water's edge is drawn.
    const float u = (d >= sh) ? (d - sh) / std::max(hw - sh, 1e-3f)
                              : (sh - d) / std::max(hw + sh, 1e-3f);
    return glm::mix(surf, bed, depthFactor(k, st, u));
}

// --- The course --------------------------------------------------------------

Course solve(Kind k, const Style& st, const std::vector<glm::vec2>& flat,
             const std::vector<float>& bias, const std::vector<int>& ptSample,
             const std::function<float(float, float)>& groundAt) {
    Course c;
    const int n = static_cast<int>(flat.size());
    if (n < 2) return c;

    // 1) The ground under the author's line, in the order they drew it.
    std::vector<float> g0(n);
    for (int i = 0; i < n; ++i)
        g0[i] = groundAt ? groundAt(flat[i].x, flat[i].y) : 0.0f;

    // 2) Which end is the source. The means of the first and last fifth rather
    //    than the two endpoints: one control point that happens to sit in a
    //    hollow must not decide which way a kilometre of river runs.
    bool rev = st.flip;
    if (st.autoFlow) {
        const int m = std::max(1, n / 5);
        float a = 0.0f, b = 0.0f;
        for (int i = 0; i < m; ++i) { a += g0[i]; b += g0[n - 1 - i]; }
        rev = (b > a);                 // the far end is higher: drawn upstream
        if (st.flip) rev = !rev;       // ...and the author says otherwise
    }

    // Everything from here on is in DOWNSTREAM order, so no later stage has to
    // remember which way round the author drew.
    c.reversed = rev;
    c.line.resize(n);
    c.ground.resize(n);
    std::vector<float> B(n);
    std::vector<glm::vec2> P(n);
    for (int i = 0; i < n; ++i) {
        const int src = rev ? (n - 1 - i) : i;
        P[i]        = flat[src];
        c.ground[i] = g0[src];
        B[i]        = (src < static_cast<int>(bias.size())) ? bias[src] : 0.0f;
    }
    c.ptSample.reserve(ptSample.size());
    if (rev) {
        for (int j = static_cast<int>(ptSample.size()) - 1; j >= 0; --j)
            c.ptSample.push_back(n - 1 - glm::clamp(ptSample[j], 0, n - 1));
    } else {
        c.ptSample = ptSample;
    }

    // 3) Arclength along the line as drawn -- needed before the wander, because
    //    the wave is a function of distance travelled.
    c.s.assign(n, 0.0f);
    for (int i = 1; i < n; ++i)
        c.s[i] = c.s[i - 1] + glm::distance(P[i], P[i - 1]);
    c.length = c.s[n - 1];

    // Everything measured off the line, re-measured. Both of the steps below
    // MOVE the line, and each of them has to leave the course consistent for the
    // next one -- a ground sampled where the water used to be is the kind of
    // mistake that only shows up two features later.
    auto remeasure = [&]() {
        for (int i = 0; i < n; ++i)
            c.ground[i] = groundAt ? groundAt(P[i].x, P[i].y) : 0.0f;
        c.s[0] = 0.0f;
        for (int i = 1; i < n; ++i)
            c.s[i] = c.s[i - 1] + glm::distance(P[i], P[i - 1]);
        c.length = c.s[n - 1];
        for (int i = 0; i < n; ++i)
            c.wander[i] = wander(c.s[i], st.meanderLength, st.seed);
    };

    // 3b) The meander. The drawn line is the VALLEY, and the water wanders
    //     inside it -- but only where the ground lets it: a gradient under the
    //     water sends it straight down the fall line instead, which is why a
    //     mountain stream is straight and a lowland river is not. The gradient it
    //     gives up by is `rapidSlope`, the same number that says where the surface
    //     breaks, so "too steep to meander" and "steep enough to be a rapid" are
    //     one statement rather than two that can disagree.
    c.wander.assign(n, 0.0f);
    for (int i = 0; i < n; ++i)
        c.wander[i] = wander(c.s[i], st.meanderLength, st.seed);

    if (st.meander > 0.01f && n > 3) {
        // The ground's own gradient over a stretch long enough not to be noise.
        // Sampled from the NATURAL ground, because the profile does not exist yet
        // -- and it must not, or the wander would depend on the bed it is about
        // to help decide.
        std::vector<float> steep(n, 0.0f);
        const float win = 20.0f;
        for (int i = 0; i < n; ++i) {
            int a = i, b = i;
            while (a > 0     && c.s[i] - c.s[a] < win) --a;
            while (b < n - 1 && c.s[b] - c.s[i] < win) ++b;
            const float run = std::max(c.s[b] - c.s[a], 1e-3f);
            steep[i] = std::fabs(c.ground[a] - c.ground[b]) / run;
        }
        const float lo = std::max(st.rapidSlope * 0.4f, 1e-4f);
        const float hi = std::max(st.rapidSlope * 2.0f, lo + 1e-4f);

        // How free the water is to wander here, 0..1 -- and SMOOTHED over the
        // meander's own scale before it is used.
        //
        // Not a nicety. This number multiplies a displacement tens of metres
        // wide, so a step in it is a step in the LINE: let the terrain's own
        // roughness switch it from 0 to 1 between two samples two metres apart
        // and the course folds back on itself hard enough to cut through its own
        // bank. A meander is a feature the length of its wavelength, so what
        // allows or forbids one has to vary on that scale too.
        std::vector<float> calm(n, 0.0f);
        for (int i = 0; i < n; ++i)
            calm[i] = 1.0f - smooth01(glm::clamp((steep[i] - lo) / (hi - lo),
                                                 0.0f, 1.0f));
        {
            const float win = std::max(st.meanderLength * 0.35f, 20.0f);
            std::vector<float> sm(n, 0.0f);
            for (int i = 0; i < n; ++i) {
                float acc = 0.0f, wsum = 0.0f;
                for (int j = i; j >= 0 && c.s[i] - c.s[j] <= win; --j) {
                    const float wj = 1.0f - (c.s[i] - c.s[j]) / win;
                    acc += calm[j] * wj; wsum += wj;
                }
                for (int j = i + 1; j < n && c.s[j] - c.s[i] <= win; ++j) {
                    const float wj = 1.0f - (c.s[j] - c.s[i]) / win;
                    acc += calm[j] * wj; wsum += wj;
                }
                sm[i] = wsum > 1e-4f ? acc / wsum : calm[i];
            }
            calm.swap(sm);
        }

        // Never so far that the channel folds back through itself: a quarter of
        // the wavelength is about where a sine's own lateral slope reaches 1.
        const float amp = std::min(st.meander, st.meanderLength * 0.25f);

        std::vector<glm::vec2> Q = P;
        for (int i = 1; i < n - 1; ++i) {
            // Held at both ends, so the course still starts and finishes at the
            // points that were clicked -- the author asked for water from HERE to
            // THERE, and only the middle is the tool's business.
            const float t    = c.s[i] / std::max(c.length, 1e-3f);
            const float ends = glm::clamp(t / 0.08f, 0.0f, 1.0f) *
                               glm::clamp((1.0f - t) / 0.08f, 0.0f, 1.0f);
            const glm::vec2 tang = P[std::min(i + 1, n - 1)] - P[std::max(i - 1, 0)];
            const float tl = glm::length(tang);
            if (tl < 1e-5f) continue;
            const glm::vec2 d = tang / tl;
            const glm::vec2 nrm(d.y, -d.x);
            Q[i] = P[i] + nrm * (amp * ends * calm[i] * c.wander[i]);
        }
        P.swap(Q);
        remeasure();
    }

    // 3c) Ease the bends. A drawn line is a polygon: it has corners, and water
    //     does not. Neither does a river turn inside a couple of its own widths
    //     -- try to make it and the sections of neighbouring stations start
    //     cutting through each other on the inside of the curve.
    //
    //     A triangle-weighted average over a window measured in METRES, not in
    //     samples: the sample spacing is the spline's business and may change,
    //     and a filter whose reach moved with it would ease a river differently
    //     depending on how densely it happened to be sampled. Held at the ends,
    //     so the course still starts and finishes where it was clicked.
    if (st.bendEase > 0.01f && n > 4) {
        // Against the width the channel AVERAGES, not the one it starts at. A
        // river that doubles on the way down does its widest turning at the far
        // end, and easing the whole course to the source width leaves exactly
        // that end under-eased -- which is where the folds were.
        const float meanWidth = std::max(st.width, 0.1f) *
                                (1.0f + std::max(st.widen, 1.0f)) * 0.5f;
        const float ease = glm::clamp(st.bendEase * meanWidth, 0.5f, 200.0f);
        std::vector<glm::vec2> S = P;
        for (int i = 1; i < n - 1; ++i) {
            glm::vec2 acc(0.0f);
            float wsum = 0.0f;
            for (int j = i; j >= 0 && c.s[i] - c.s[j] <= ease; --j) {
                const float wj = 1.0f - (c.s[i] - c.s[j]) / ease;
                acc += P[j] * wj; wsum += wj;
            }
            for (int j = i + 1; j < n && c.s[j] - c.s[i] <= ease; ++j) {
                const float wj = 1.0f - (c.s[j] - c.s[i]) / ease;
                acc += P[j] * wj; wsum += wj;
            }
            if (wsum <= 1e-4f) continue;
            const float t = c.s[i] / std::max(c.length, 1e-3f);
            const float hold = glm::clamp(t / 0.05f, 0.0f, 1.0f) *
                               glm::clamp((1.0f - t) / 0.05f, 0.0f, 1.0f);
            S[i] = glm::mix(P[i], acc / wsum, hold);
        }
        P.swap(S);
        remeasure();
    }

    // 4) The profile. Hug the ground where the ground already falls, cut where it
    //    does not, and never let a stretch rise -- which is the whole promise of
    //    this module and the reason the author does not have to place heights.
    //
    //    `maxCut` is a safety rail rather than a look: a line drawn up a mountain
    //    would otherwise cut a two-hundred-metre notch and a carve over a million
    //    cells. Where it bites the profile HAS to rise, and that is reported
    //    rather than hidden -- water flowing backwards is the one thing the panel
    //    must be able to say out loud.
    std::vector<float> H(n);
    H[0] = c.ground[0] + B[0];
    for (int i = 1; i < n; ++i) {
        const float ds = std::max(c.s[i] - c.s[i - 1], 1e-4f);
        H[i] = std::min(c.ground[i] + B[i], H[i - 1] - st.minSlope * ds);
        const float floorH = c.ground[i] - st.maxCut;
        if (H[i] < floorH) {
            H[i] = floorH;
            if (H[i] > H[i - 1] + 1e-4f) c.uphill = true;
        }
    }

    // 5) Falls. A run of stations steeper than `fallSlope` is a step -- but only
    //    if it actually drops far enough to be one, or every boulder-sized dip in
    //    the ground noise becomes a waterfall.
    std::vector<char> fall(n, 0), locked(n, 0);
    for (int i = 1; i < n; ++i) {
        const float ds = std::max(c.s[i] - c.s[i - 1], 1e-4f);
        if ((H[i - 1] - H[i]) / ds > st.fallSlope) fall[i] = 1;
    }
    for (int i = 1; i < n; ) {
        if (!fall[i]) { ++i; continue; }
        int a = i;
        while (i < n && fall[i]) ++i;
        const int b = i - 1;                       // last station of the run
        if (H[a - 1] - H[b] < st.fallMin) {        // too short: it is a rapid
            for (int j = a; j <= b; ++j) fall[j] = 0;
            continue;
        }
        ++c.falls;
        // The pool above the lip: water arriving at a fall is calm, and without
        // that flat the drop reads as a slope rather than as an edge.
        const float lip = H[a - 1];
        for (int j = a - 1; j >= 0 && c.s[a - 1] - c.s[j] <= st.poolLength; --j) {
            H[j] = lip; locked[j] = 1;
        }
        // ...and the plunge pool below it, held only while the ground is still
        // under the water. Past that the pool would be perched on a hillside and
        // the carve would have to build it a dam.
        const float base = H[b];
        for (int j = b; j < n && c.s[j] - c.s[b] <= st.poolLength; ++j) {
            if (c.ground[j] < base - (st.depth + st.plunge)) break;
            H[j] = base; locked[j] = 1;
        }
        for (int j = a; j <= b; ++j) locked[j] = 1;
    }

    // 6) Smooth the rest. The gradient is what decides where the water breaks, so
    //    an unsmoothed profile speckles foam wherever the ground noise dips. Falls
    //    and pools are left alone -- rounding a lip is exactly what must not
    //    happen to it.
    std::vector<float> tmp(n);
    for (int pass = 0; pass < std::max(0, st.smooth); ++pass) {
        tmp = H;
        for (int i = 1; i < n - 1; ++i) {
            if (locked[i] || locked[i - 1] || locked[i + 1]) continue;
            tmp[i] = 0.25f * H[i - 1] + 0.5f * H[i] + 0.25f * H[i + 1];
        }
        H.swap(tmp);
        for (int i = 1; i < n; ++i) H[i] = std::min(H[i], H[i - 1]);
    }

    // 7) Widths, the plunge scoop, and the surface line itself.
    c.half.resize(n);
    c.deep.resize(n);
    c.shift.assign(n, 0.0f);
    c.bed.resize(n);
    c.dir.resize(n);
    c.slope.assign(n, 0.0f);
    c.white.assign(n, 0.0f);
    const float invLen = c.length > 1e-3f ? 1.0f / c.length : 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = c.s[i] * invLen;
        // Downstream growth. The depth grows with it but more slowly -- a river
        // that doubles its width gathers about half again its depth, which is
        // roughly what a channel's own hydraulics settle at and is anyway what
        // stops a wide river from also being a trench.
        const float grow = glm::mix(1.0f, std::max(st.widen, 0.05f), t);
        // The pool-and-riffle sequence, off the SAME wave the wander uses at
        // twice its frequency: |wave| peaks at each bend apex (a pool -- narrow,
        // deep, slow) and crosses zero at each crossing between them (a riffle --
        // wide, shallow, quick). +1 is a riffle, -1 a pool.
        //
        // The two swing OPPOSITE ways because they are the same water: a stretch
        // that spreads out has to get shallower, and one that is squeezed has to
        // get deeper. Driving them from one number is what keeps that true.
        // 1 - 2w^2 rather than 1 - 2|w|. The two agree at the crossings (+1) and
        // at the apexes (-1), and only one of them is SMOOTH there: |w| has a
        // corner at every zero crossing, and a corner in the width is a visible
        // kink in the waterline every half wavelength for the whole length of
        // the river.
        const float riffle = 1.0f - 2.0f * c.wander[i] * c.wander[i];
        c.half[i] = 0.5f * std::max(st.width, 0.1f) * grow *
                    (1.0f + glm::clamp(st.widthVary, 0.0f, 0.9f) * riffle);
        c.deep[i] = std::max(st.depth, 0.01f) * std::sqrt(grow) *
                    (1.0f - glm::clamp(st.depthVary, 0.0f, 0.9f) * riffle);
        // The deep line runs against the OUTER bank, not down the middle -- and
        // the outer bank of a bend is the side the wander pushed the channel
        // towards, which is why this is the same number again rather than a
        // curvature measured back off the geometry.
        c.shift[i] = glm::clamp(st.bendScour, 0.0f, 0.9f) * c.half[i] * c.wander[i];
        c.line[i] = glm::vec3(P[i].x, H[i], P[i].y);
        const glm::vec2 a = P[std::max(i - 1, 0)];
        const glm::vec2 b = P[std::min(i + 1, n - 1)];
        const glm::vec2 d = b - a;
        const float dl = glm::length(d);
        c.dir[i] = dl > 1e-5f ? d / dl : glm::vec2(0.0f, 1.0f);
        c.maxCut = std::max(c.maxCut, c.ground[i] - H[i]);
    }
    // A channel cannot be wider than its own bend will carry. Past a turning
    // radius of about one half-width the inner bank crosses itself: the section
    // on the inside of the curve folds through the section of the station
    // before it, the cut eats a hole where there should be a point bar, and the
    // water surface self-intersects.
    //
    // Easing the LINE cannot fully prevent that, and should not try -- a corner
    // an author drew between two points sixty metres apart is a corner they
    // meant, and rounding it away to fit the widest part of the river would move
    // the river off the line instead. So the CHANNEL yields rather than the
    // course: it pinches where it has to, which is also what a real one does
    // when it is forced through a tight meander.
    //
    // The 0.8 is what makes the guarantee arithmetic rather than hopeful: with
    // half <= 0.8 R, the radius is always at least 0.625 of the full width, and
    // no drawn line whatsoever can invert a section.
    for (int i = 0; i < n; ++i) {
        const int a = std::max(i - 2, 0), b = std::min(i + 2, n - 1);
        const float run = std::max(c.s[b] - c.s[a], 1e-3f);
        const float dot = glm::clamp(glm::dot(c.dir[a], c.dir[b]), -1.0f, 1.0f);
        const float turn = std::acos(dot) / run;          // 1 / radius
        if (turn < 1e-5f) continue;
        const float carry = 0.8f / turn;                  // 0.8 R
        // Never below a quarter of what was asked for: a channel that vanished
        // at a bend would be a worse answer than one that is merely narrow.
        c.half[i] = std::max(std::min(c.half[i], carry), c.half[i] * 0.25f);
    }

    c.drop = H[0] - H[n - 1];

    // The surface distance, now that the heights are settled.
    c.sSurf.assign(n, 0.0f);
    for (int i = 1; i < n; ++i)
        c.sSurf[i] = c.sSurf[i - 1] + glm::distance(c.line[i], c.line[i - 1]);

    // The scoop under a fall, faded out downstream -- a plunge pool is darker
    // than the stretch above it, and that darkness is most of what says the
    // water fell a long way to get there.
    std::vector<float> scoop(n, 0.0f);
    for (int i = 1; i < n; ++i) {
        if (!fall[i]) continue;
        for (int j = i; j < n && c.s[j] - c.s[i] <= st.poolLength; ++j) {
            const float f = 1.0f - (c.s[j] - c.s[i]) / std::max(st.poolLength, 0.1f);
            scoop[j] = std::max(scoop[j], f);
        }
    }
    for (int i = 0; i < n; ++i) {
        c.deep[i] += st.plunge * scoop[i];
        c.bed[i]   = H[i] - c.deep[i];
    }

    // 8) Where it breaks white. From the profile's own gradient, so the foam is
    //    always on the stretch the water is actually racing down -- plus a jitter
    //    so a long even rapid is not one flat sheet of white.
    for (int i = 1; i < n; ++i) {
        const float ds = std::max(c.s[i] - c.s[i - 1], 1e-4f);
        c.slope[i] = std::max((H[i - 1] - H[i]) / ds, 0.0f);
    }
    c.slope[0] = c.slope.size() > 1 ? c.slope[1] : 0.0f;
    const float lo = std::max(st.rapidSlope, 1e-4f);
    const float hi = std::max(st.fallSlope, lo + 1e-4f);
    for (int i = 0; i < n; ++i) {
        float w = smooth01(glm::clamp((c.slope[i] - lo) / (hi - lo), 0.0f, 1.0f));
        if (fall[i]) w = 1.0f;
        const float j = hash1(st.seed * 7919u + static_cast<unsigned>(i));
        c.white[i] = glm::clamp(w * (0.75f + 0.5f * j), 0.0f, 1.0f);
    }
    // ...and a riffle breaks a little even where the gradient alone would not:
    // shallow fast water over a bed is exactly what a riffle is, and a lowland
    // river with no white on it anywhere reads as a canal. Scaled by how shallow
    // the sequence actually made it, so a course with no depth variation gets
    // none of this.
    {
        const float base = std::max(st.depth, 0.01f);
        for (int i = 0; i < n; ++i) {
            const float shallowness = glm::clamp(1.0f - c.deep[i] / base, 0.0f, 1.0f);
            c.white[i] = std::max(c.white[i], 0.40f * shallowness);
        }
    }

    // Foam does not stop where the slope does: it is carried on and dies of old
    // age a few metres downstream. Without this every rapid ends on a hard line.
    for (int i = 1; i < n; ++i) {
        const float ds = c.s[i] - c.s[i - 1];
        c.white[i] = std::max(c.white[i], c.white[i - 1] - ds / 9.0f);
    }
    // ...and a touch of it reaches back upstream, where the water is already
    // being drawn into the drop.
    for (int i = n - 2; i >= 0; --i) {
        const float ds = c.s[i + 1] - c.s[i];
        c.white[i] = std::max(c.white[i], c.white[i + 1] - ds / 3.0f);
    }

    // The coordinate the surface pattern is drawn in: surface distance divided
    // by the local speed, so it is a TRAVEL TIME. See Course::flow for why the
    // obvious alternative -- scrolling faster where the water is faster -- makes
    // the texture run backwards after a few minutes of play.
    c.flow.assign(n, 0.0f);
    for (int i = 1; i < n; ++i) {
        const float ds = std::max(c.sSurf[i] - c.sSurf[i - 1], 0.0f);
        const float v  = 1.0f + (c.white[i] + c.white[i - 1]);   // 1 .. 3
        c.flow[i] = c.flow[i - 1] + ds / v;
    }

    return c;
}

// --- The surface -------------------------------------------------------------

Surface surface(Kind k, const Style& st, const Course& c) {
    Surface out;
    const int n = static_cast<int>(c.line.size());
    if (n < 2) return out;

    // Columns across the channel. Enough that the depth ramp and the bank foam
    // read as curves rather than facets, and no more: a kilometre of brook is
    // already tens of thousands of vertices at three columns.
    float maxHalf = 0.0f;
    for (float h : c.half) maxHalf = std::max(maxHalf, h);
    int cols = glm::clamp(3 + static_cast<int>(maxHalf * 0.9f), 5, 11);
    if ((cols & 1) == 0) ++cols;

    fitzel::MeshData& md = out.data;
    md.vertices.reserve(static_cast<std::size_t>(n) * cols);
    md.indices.reserve(static_cast<std::size_t>(n - 1) * (cols - 1) * 6);

    glm::vec3 lo(1e30f), hi(-1e30f);
    for (int i = 0; i < n; ++i) {
        const glm::vec2 d    = c.dir[i];
        const glm::vec3 side(d.y, 0.0f, -d.x);
        // The 3D tangent, so a fall's curtain gets a normal that leans with it
        // instead of one pointing at the sky.
        const glm::vec3 a  = c.line[std::max(i - 1, 0)];
        const glm::vec3 b  = c.line[std::min(i + 1, n - 1)];
        glm::vec3 tan3 = b - a;
        if (glm::length(tan3) < 1e-5f) tan3 = glm::vec3(d.x, 0.0f, d.y);
        tan3 = glm::normalize(tan3);
        glm::vec3 nrm = glm::normalize(glm::cross(side, tan3));
        if (nrm.y < 0.0f) nrm = -nrm;

        // A hair inside the waterline: the carve puts the ground at exactly the
        // water surface there, and two coplanar surfaces meeting edge-on is a
        // z-fight the whole length of the bank.
        const float hw = c.half[i] * 0.99f;
        const float sh = glm::clamp(c.shift[i], -c.half[i] * 0.85f,
                                     c.half[i] * 0.85f);
        for (int j = 0; j < cols; ++j) {
            const float u = -1.0f + 2.0f * static_cast<float>(j) /
                                    static_cast<float>(cols - 1);
            fitzel::Vertex v;
            v.position = c.line[i] + side * (u * hw);
            v.normal   = nrm;
            // METRES, not a 0..1 wrap: across the channel and along it. The
            // ripples are a physical size, and a river that doubles its width
            // downstream must not stretch them with it -- which is exactly what
            // a normalised u would do.
            v.uv       = glm::vec2(u * hw, c.flow[i]);
            // Everything else the river shader needs, in the four floats every
            // vertex already carries for the terrain's paint weights (see
            // RiverGen.hpp). `half` rides along so the shader can put the bank
            // foam a fixed number of METRES in from an edge that moves.
            // METRES of water under this vertex, not a 0..1 fraction: the depth
            // varies station to station now, so a fraction would need the shader
            // to be told a depth it no longer has one of. The section is measured
            // from the DEEP LINE, which is where the bed was actually cut.
            const float dm = u * hw;
            const float uu = (dm >= sh) ? (dm - sh) / std::max(c.half[i] - sh, 1e-3f)
                                        : (sh - dm) / std::max(c.half[i] + sh, 1e-3f);
            // w is the local speed in m/s, and it is there to be READ, never to
            // be multiplied by a clock -- doing that is what made the pattern
            // tear (see Course::flow).
            v.paint    = glm::vec4(depthFactor(k, st, uu) * c.deep[i],
                                   c.white[i], hw,
                                   st.flowSpeed * (1.0f + 2.0f * c.white[i]));
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
            md.vertices.push_back(v);
        }
    }
    for (int i = 0; i + 1 < n; ++i) {
        const std::uint32_t r0 = static_cast<std::uint32_t>(i) * cols;
        const std::uint32_t r1 = r0 + cols;
        for (int j = 0; j + 1 < cols; ++j) {
            const std::uint32_t a = r0 + j, b = r0 + j + 1;
            const std::uint32_t d = r1 + j, e = r1 + j + 1;
            md.indices.insert(md.indices.end(), {a, d, b, b, d, e});
        }
    }
    out.lo = lo;
    out.hi = hi;
    out.verts = static_cast<int>(md.vertices.size());
    return out;
}

// --- Bank dressing -----------------------------------------------------------

namespace {

// Find-or-create one material by name, re-applying its look either way -- so
// re-colouring re-skins every course already using it, which is the point of
// sharing them. The same helper SplineGen and BuildingGen keep locally, and
// local here for the same reason: it is a dozen lines, and a shared one would
// drag a header between three modules that otherwise know nothing about each
// other.
fitzel::AssetId ensureMaterial(std::vector<MaterialDef>& mats,
                               const std::string& name, glm::vec3 albedo,
                               float refl, float rough) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo       = albedo;
        m.reflectivity = refl;
        m.roughness    = rough;
        if (!m.assetId.valid()) m.assetId = fitzel::AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = fitzel::AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = refl;
    md.roughness    = rough;
    mats.push_back(md);
    return md.assetId;
}

void addTri(fitzel::MeshData& md, const glm::vec3& a, const glm::vec3& b,
            const glm::vec3& c, glm::vec2 ua, glm::vec2 ub, glm::vec2 uc) {
    const glm::vec3 e1 = b - a, e2 = c - a;
    const glm::vec3 cr = glm::cross(e1, e2);
    if (glm::dot(cr, cr) < 1e-14f) return;         // degenerate: nothing to draw
    const glm::vec3 nrm = glm::normalize(cr);
    const std::uint32_t base = static_cast<std::uint32_t>(md.vertices.size());
    fitzel::Vertex v;
    v.normal = nrm;
    v.position = a; v.uv = ua; md.vertices.push_back(v);
    v.position = b; v.uv = ub; md.vertices.push_back(v);
    v.position = c; v.uv = uc; md.vertices.push_back(v);
    md.indices.insert(md.indices.end(), {base, base + 1, base + 2});
}

// A boulder: fourteen points on a jittered sphere -- the eight corners of a
// cube and the six face centres -- fanned into twenty-four flat-shaded
// triangles. No texture and no smoothing, which is what a water-worn stone seen
// from three metres actually needs.
//
// The face centres are the whole trick and are worth the six extra points. With
// the corners alone the shape is a warped cube, and a warped cube does not read
// as a boulder at any distance: the flat quads catch the light as flat quads and
// the silhouette stays straight-edged. Pushing a point out of the middle of each
// one breaks both.
void addStone(fitzel::MeshData& md, const glm::vec3& at, float r, float squash,
              float yaw, unsigned seed) {
    const float ca = std::cos(yaw), sa = std::sin(yaw);
    auto place = [&](glm::vec3 dir, unsigned k) {
        glm::vec3 c = glm::normalize(dir) *
                      (r * (0.74f + 0.50f * hash1(seed * 131u + k)));
        c.y *= squash;
        return at + glm::vec3(c.x * ca - c.z * sa, c.y, c.x * sa + c.z * ca);
    };
    glm::vec3 p[8], f[6];
    for (int i = 0; i < 8; ++i)
        p[i] = place(glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                               (i & 4) ? 1.0f : -1.0f), static_cast<unsigned>(i));
    static const glm::vec3 axis[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0},
                                      {0, 1, 0},  {-1, 0, 0}, {1, 0, 0}};
    for (int i = 0; i < 6; ++i)
        f[i] = place(axis[i], static_cast<unsigned>(8 + i));
    // The cube's six quads by corner index, wound so every face points outward,
    // each fanned to its own face point.
    static const int q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int i = 0; i < 6; ++i)
        for (int e = 0; e < 4; ++e)
            addTri(md, p[q[i][e]], p[q[i][(e + 1) & 3]], f[i],
                   {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f});
}

// A reed: a tapered blade leaning off vertical, built as solid geometry rather
// than a cutout quad. Deliberately -- a cutout needs an alpha texture the
// project may not have, and a reed bed made of untextured rectangles is worse
// than no reed bed at all. Wound both ways, so it is there from either bank.
void addReed(fitzel::MeshData& md, const glm::vec3& foot, float h, float w,
             float yaw, float lean) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 dir(std::cos(yaw), 0.0f, std::sin(yaw));
    const glm::vec3 side = glm::cross(up, dir) * (w * 0.5f);
    const glm::vec3 tip  = foot + up * h + dir * (lean * h);
    const glm::vec3 mid  = foot + up * (h * 0.55f) + dir * (lean * h * 0.2f);
    const glm::vec3 a = foot - side, b = foot + side;
    const glm::vec3 c = mid - side * 0.45f, d = mid + side * 0.45f;
    for (int s = 0; s < 2; ++s) {
        const bool flip = (s == 1);
        const glm::vec3 A = flip ? b : a, B = flip ? a : b;
        const glm::vec3 C = flip ? d : c, D = flip ? c : d;
        addTri(md, A, B, D, {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.55f});
        addTri(md, A, D, C, {0.0f, 0.0f}, {1.0f, 0.55f}, {0.0f, 0.55f});
        addTri(md, C, D, tip, {0.0f, 0.55f}, {1.0f, 0.55f}, {0.5f, 1.0f});
    }
}

} // namespace

Dressing ensureDressing(std::vector<MaterialDef>& materials, const Style& st) {
    Dressing d;
    // Shared by name rather than per course: a valley with six brooks in it
    // wants six brooks and two materials, not twelve.
    d.stone = ensureMaterial(materials, "Water Stone", st.stoneColor, 0.03f, 0.88f);
    d.reed  = ensureMaterial(materials, "Water Reed",  st.reedColor,  0.0f,  0.95f);
    return d;
}

std::vector<Batch> dressing(Kind k, const Style& st, const Course& c,
                            const Dressing& mats, int maxPieces) {
    std::vector<Batch> out;
    const int n = static_cast<int>(c.line.size());
    if (n < 2 || c.length < 1.0f) return out;
    const bool wantStones = st.stones > 0.01f;
    const bool wantReeds  = st.reeds  > 0.01f;
    if (!wantStones && !wantReeds) return out;

    Batch stone; stone.material = mats.stone;
    Batch reed;  reed.material  = mats.reed;
    glm::vec3 slo(1e30f), shi(-1e30f), rlo(1e30f), rhi(-1e30f);
    int budget = std::max(maxPieces, 1);

    // Walk the course at a fixed step and roll for a piece at each station, so
    // the density is per metre of CHANNEL -- a river that widens downstream does
    // not thin its own bank out as it goes.
    const float step = 1.0f;
    unsigned draw = st.seed * 2654435761u + 17u;
    auto roll = [&draw]() { return hash1(draw++); };
    int cursor = 0;
    for (float s = 0.0f; s < c.length && budget > 0; s += step) {
        while (cursor + 1 < n - 1 && c.s[cursor + 1] < s) ++cursor;
        const int i = std::min(cursor, n - 2);
        const float span = std::max(c.s[i + 1] - c.s[i], 1e-4f);
        const float t = glm::clamp((s - c.s[i]) / span, 0.0f, 1.0f);

        const glm::vec3 mid  = glm::mix(c.line[i], c.line[i + 1], t);
        const float     half = glm::mix(c.half[i], c.half[i + 1], t);
        const float     bed  = glm::mix(c.bed[i],  c.bed[i + 1],  t);
        const float     shift= glm::mix(c.shift[i], c.shift[i + 1], t);
        const glm::vec2 dir  = glm::mix(c.dir[i],  c.dir[i + 1],  t);
        const glm::vec3 side(dir.y, 0.0f, -dir.x);

        if (wantStones && roll() < st.stones * step / 100.0f) {
            // Anywhere from mid-channel to a little past the waterline. A
            // boulder in the stream is the point; one sitting on the bank beside
            // it is what stops the row of them reading as a kerb.
            const float d = (roll() * 2.0f - 1.0f) *
                            (half + std::max(st.stoneSpread, 0.0f));
            const float ground = sectionHeight(k, st, d, half, shift, mid.y, bed,
                                              mid.y);
            const float r = std::max(st.stoneSize, 0.02f) * (0.5f + 1.1f * roll());
            const glm::vec3 at = mid + side * d +
                                 glm::vec3(0.0f, ground - mid.y + r * 0.30f, 0.0f);
            // ...and part buried: a stone resting exactly on the surface reads
            // as one that was dropped there this morning.
            addStone(stone.data, at - glm::vec3(0.0f, r * 0.55f, 0.0f), r,
                     0.62f + 0.30f * roll(), roll() * 6.2831853f, draw);
            slo = glm::min(slo, at - glm::vec3(r * 2.0f));
            shi = glm::max(shi, at + glm::vec3(r * 2.0f));
            --budget;
        }

        if (wantReeds && roll() < st.reeds * step / 100.0f) {
            // Only in the shallows, near a bank, and not in broken water. Reeds
            // do not grow in the middle of a river and one standing in a rapid is
            // a mistake the eye finds instantly -- so this asks the SECTION how
            // deep it is at that offset rather than trusting the nominal depth.
            const float d = (roll() < 0.5f ? -1.0f : 1.0f) *
                            (0.55f + 0.45f * roll()) * half;
            const float ground = sectionHeight(k, st, d, half, shift, mid.y, bed,
                                              mid.y);
            const float water  = mid.y - ground;
            if (water > 0.0f && water < std::max(st.reedDepth, 0.02f) &&
                c.white[i] < 0.35f) {
                const glm::vec3 foot = mid + side * d +
                                       glm::vec3(0.0f, ground - mid.y, 0.0f);
                const int blades = 3 + static_cast<int>(roll() * 5.0f);
                for (int b = 0; b < blades && budget > 0; ++b, --budget) {
                    const float a  = roll() * 6.2831853f;
                    const float rr = roll() * 0.35f;
                    const glm::vec3 f = foot + glm::vec3(std::cos(a) * rr, 0.0f,
                                                         std::sin(a) * rr);
                    const float h = std::max(st.reedHeight, 0.05f) *
                                    (0.6f + 0.7f * roll());
                    addReed(reed.data, f, h, 0.035f + 0.03f * roll(),
                            roll() * 6.2831853f, (roll() - 0.5f) * 0.35f);
                    rlo = glm::min(rlo, f - glm::vec3(0.4f, 0.0f, 0.4f));
                    rhi = glm::max(rhi, f + glm::vec3(0.4f, h * 1.4f, 0.4f));
                }
            }
        }
    }

    if (!stone.data.vertices.empty()) {
        stone.lo = slo; stone.hi = shi;
        out.push_back(std::move(stone));
    }
    if (!reed.data.vertices.empty()) {
        reed.lo = rlo; reed.hi = rhi;
        out.push_back(std::move(reed));
    }
    return out;
}

// --- Spray -------------------------------------------------------------------

std::vector<SprayPoint> spray(const Style& st, const Course& c) {
    std::vector<SprayPoint> out;
    const int n = static_cast<int>(c.line.size());
    if (n < 2) return out;
    // One emitter every few metres of loud water, so a long cascade throws spray
    // along its whole length and a single step throws it at the one place it
    // lands. Quiet stretches contribute nothing at all -- an emitter that never
    // emits still costs a pool slot every frame.
    const float spacing = std::max(2.5f, st.width * 0.6f);
    float next = -1e9f;
    for (int i = 1; i < n; ++i) {
        if (c.white[i] < 0.55f || c.slope[i] < st.rapidSlope) continue;
        if (c.s[i] - next < spacing) continue;
        next = c.s[i];
        SprayPoint sp;
        sp.pos      = c.line[i];
        sp.dir      = glm::normalize(glm::vec3(c.dir[i].x, -0.25f, c.dir[i].y));
        sp.strength = glm::clamp(c.white[i] * (0.4f + c.slope[i] * 1.4f), 0.0f, 1.0f);
        sp.width    = c.half[i] * 1.6f;
        out.push_back(sp);
    }
    return out;
}

} // namespace rivergen
