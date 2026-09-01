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
    // Not const: the falls are resampled below (step 6b), which is the one stage
    // that adds stations rather than only reading them.
    int n = static_cast<int>(flat.size());
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


    // 6b) The falls, resolved. -------------------------------------------------
    //
    // Up to here a fall is two stations and a number. The samples are two metres
    // apart IN PLAN and a cliff is not, so the whole height of the drop happens
    // between one station and the next -- and a surface drawn over that does
    // exactly what the profile does: it turns a right angle at the lip, runs down
    // the rock as a flat ribbon, and turns another one at the bottom. Both of
    // those creases are the first thing anybody sees, and no amount of shading
    // fixes a crease.
    //
    // What water does at a lip is not a corner. It arrives MOVING, and a moving
    // thing that runs out of ledge follows a parabola: y = -g x^2 / 2v^2, from
    // the lip, in the plan direction it was already travelling. So the surface
    // over a fall is that parabola for as long as it stands ABOVE the ground the
    // profile had, and where the rock comes back up to meet it -- a cascade
    // rather than a free fall -- the rock wins again.
    //
    // Taking the LARGER of the two is what makes that one rule instead of two
    // cases, and it is what buys the lip its rounding for nothing: a parabola
    // leaves its start horizontal, so it is already tangent to the pool above the
    // lip and there is no corner left to round. The same maximum, smoothed, eases
    // the jet into the plunge pool at the bottom instead of planting it there.
    //
    // Both of those need STATIONS to be drawn with, and a two-metre plan spacing
    // has none to spare -- so the fall's own stretch is resampled, uniformly in
    // plan, which for a parabola is uniform in curvature: fine where it bends
    // over the lip, coarse down the straight part of the drop where two points
    // would do.
    //
    // The price of the parabola is that the water is then somewhere the ground is
    // not, which the carve has to be told or it would build a ramp of earth under
    // a jet hanging in mid-air. That is what Course::air is: how far this
    // station's water stands clear of the ground it came off.
    c.air.assign(n, 0.0f);
    {
        constexpr float kG = 9.81f;
        // How fast the water goes over a lip. Critical flow -- a channel at a
        // ledge accelerates to it whatever it was doing before -- with the
        // authored current as a floor, so a mill race throws its water further
        // than a pond outlet does and neither of them needs a knob for it.
        const float vLip = std::max(std::max(st.current, 0.6f),
                                    std::sqrt(kG * glm::clamp(st.depth, 0.05f, 4.0f)));
        // Every step in the FINISHED profile, which is not the same list as the
        // falls found in step 5 and has to be re-derived here rather than reused.
        //
        // Two reasons. The first is that step 5 threw away the short ones: that
        // demotion is about the POOLS -- a knee-high drop does not deserve a flat
        // above it and a plunge below -- and it has no business deciding whether
        // the surface may be a curve. A half-metre drop over half a metre of
        // ground is a right angle in the water exactly like a fifty-metre one is,
        // and a hillside of them is the staircase a brook comes out as.
        //
        // The second is subtler and was the actual bug. Step 5 WRITES to the
        // profile: it flattens a pool above every lip, and that flat lands in the
        // middle of whatever steep run happened to be there -- so a stretch that
        // was one long fall becomes fall, shelf, fall, and the second half of it
        // is no longer the start of anything. Read the old flags and that second
        // lip has no jet, keeps its right angle, and is the one crease left in a
        // river where every other one was rounded.
        std::vector<char> ledge(n, 0);
        for (int i = 1; i < n; ++i) {
            const float ds = std::max(c.s[i] - c.s[i - 1], 1e-4f);
            if ((H[i - 1] - H[i]) / ds > st.fallSlope) ledge[i] = 1;
        }
        // One jet per step: where it leaves, how high, how hard it bends, and how
        // far downstream it gets before it has fallen the whole drop.
        struct Jet { float s0, y0, k, reach, drop, span; };
        std::vector<Jet> jets;
        for (int i = 1; i < n; ++i) {
            if (!ledge[i] || ledge[i - 1]) continue;    // first segment of a run
            int b = i;
            while (b + 1 < n && ledge[b + 1]) ++b;
            Jet j;
            j.drop  = std::max(H[i - 1] - H[b], 0.05f);
            j.s0    = c.s[i - 1];
            j.y0    = H[i - 1];
            j.k     = kG / (2.0f * vLip * vLip);
            j.reach = vLip * std::sqrt(2.0f * j.drop / kG);
            // How far downstream this step is anybody's business: the flight, or
            // the whole run of steep ground, whichever is longer, plus a margin.
            //
            // The two are different things and both matter. A vertical ledge is
            // over in a metre and the water flies for ten; a cascade runs down a
            // hundred metres of rock and the jet only covers its first few. The
            // far end of THAT is a corner too -- the place a long chute flattens
            // out into a pool -- and it needs stations for the same reason the
            // lip does.
            j.span  = std::max(j.reach * 1.9f, (c.s[b] - j.s0) + 1.5f);
            jets.push_back(j);
        }

        if (!jets.empty()) {
            // The stations the jets need, merged into the ones the line already
            // had. Uniform in plan, which for a parabola is uniform in TURNING --
            // the arc bends hardest at the lip and is nearly straight by the
            // bottom, and a plan-uniform spacing puts the samples where the bend
            // is without having to be told.
            //
            // Half again past the reach, because the landing needs stations too:
            // the arc stops being the answer where it meets the pool, and a
            // transition with no station in it is a corner however smoothly it
            // was computed.
            std::vector<float> S = c.s;
            // A budget over the whole course, not per step. A quarter-metre
            // station wherever water is falling is cheap on one waterfall and is
            // not cheap on a line drawn straight down a mountainside of ledges --
            // that is thousands of stations, each of them a row of the surface
            // strip and a handful of cells of the carve. Where the course asks
            // for more than this, every step is thinned by the same factor, so
            // what gets coarser is the whole river rather than whichever falls
            // happened to be solved last.
            constexpr int kMaxJetStations = 4000;
            int want = 0;
            for (const Jet& j : jets)
                want += glm::clamp(
                    static_cast<int>(std::lround(j.span / 0.25f)), 4, 240);
            const float thin = want > kMaxJetStations
                             ? static_cast<float>(kMaxJetStations) / want : 1.0f;
            for (const Jet& j : jets) {
                const int m = glm::clamp(
                    static_cast<int>(std::lround(j.span / 0.25f * thin)), 4, 240);
                for (int q = 1; q <= m; ++q)
                    S.push_back(j.s0 + j.span * static_cast<float>(q) /
                                       static_cast<float>(m));
            }
            std::sort(S.begin(), S.end());
            {
                std::vector<float> keep;
                keep.reserve(S.size());
                for (float v : S) {
                    if (v < 0.0f || v > c.length) continue;
                    // Never two stations a few millimetres apart: a sliver
                    // segment carries a full station's worth of drop over no
                    // ground at all, which is a corner by arithmetic.
                    if (!keep.empty() && v - keep.back() < 0.08f) continue;
                    keep.push_back(v);
                }
                if (keep.empty() || keep.back() < c.length - 1e-4f)
                    keep.push_back(c.length);
                S.swap(keep);
            }

            // The old line read at an arbitrary plan distance. Everything the
            // resample carries over -- the position, the author's bias -- comes
            // through here, so the new stations sit exactly ON the old polyline
            // rather than near it.
            int walk = 0;
            auto at = [&](float sv, float& t) {
                while (walk + 1 < n - 1 && c.s[walk + 1] < sv) ++walk;
                while (walk > 0 && c.s[walk] > sv) --walk;
                const float span = std::max(c.s[walk + 1] - c.s[walk], 1e-6f);
                t = glm::clamp((sv - c.s[walk]) / span, 0.0f, 1.0f);
            };

            const int m = static_cast<int>(S.size());
            std::vector<glm::vec2> P2(m);
            std::vector<float>     H2(m), B2(m), A2(m, 0.0f), G2(m), R2(m);
            std::vector<char>      zone(m, 0);
            for (int i = 0; i < m; ++i) {
                float t = 0.0f;
                at(S[i], t);
                P2[i] = glm::mix(P[walk], P[walk + 1], t);
                B2[i] = glm::mix(B[walk], B[walk + 1], t);
                const float rock = glm::mix(H[walk], H[walk + 1], t);
                R2[i] = rock;
                float y = rock;
                for (const Jet& j : jets) {
                    const float x = S[i] - j.s0;
                    // A metre of margin either side, so the rounding pass below
                    // reaches the pool the jet lands in and not just the jet.
                    if (x > -1.0f && x < j.span + 1.0f) zone[i] = 1;
                    if (x <= 0.0f || x > j.reach * 1.9f) continue;
                    const float jet = j.y0 - j.k * x * x;
                    if (jet <= y - 3.0f) continue;
                    // Rounded where the two meet, so the jet neither snaps onto
                    // the rock face nor lands on the pool with a crease. The
                    // radius grows with the fall: a two-metre step is rounded in
                    // centimetres, a fifty-metre one over a couple of metres.
                    //
                    // ...and it grows again with how steeply the arc is falling
                    // where it meets, because the radius is a height and what has
                    // to be rounded is an ANGLE. Two metres of height at the lip
                    // is two metres of ground; the same two metres where the jet
                    // is dropping four to one is half a metre of ground, which is
                    // one station, which is a crease. The cap is what keeps the
                    // smoothing honest: a smooth maximum lifts the result by at
                    // most an eighth of its radius, so this one can float the
                    // water no more than about forty centimetres over its pool.
                    // A metre of GROUND, expressed in the height units the two
                    // are compared in. That conversion is the whole trick: the
                    // radius is a height and what has to be rounded is an ANGLE,
                    // so a fixed height rounds a lip generously and a landing
                    // where the jet drops four to one not at all. At the lip the
                    // slope is zero and so is the radius, which is right -- a
                    // parabola leaves horizontally and has nothing to round.
                    //
                    // The cap keeps it honest: a smooth maximum lifts its result
                    // by at most an eighth of the radius, so this can float the
                    // water half a metre over its plunge pool and no more --
                    // which is about what the boil at the foot of a fall stands
                    // proud by anyway.
                    const float slope = 2.0f * j.k * x;
                    const float r = glm::clamp(slope * 1.0f, 0.30f, 4.0f);
                    const float h = glm::clamp(0.5f + 0.5f * (jet - y) / r,
                                               0.0f, 1.0f);
                    const float sm = glm::mix(y, jet, h) + r * h * (1.0f - h) * 0.5f;
                    if (sm > y) y = sm;
                }
                H2[i] = y;
            }

            // What the smooth maximum cannot do on its own: round the LANDING.
            //
            // It works in height, and at the foot of a fall the arc and the pool
            // cross so steeply that even a generous height radius is a quarter of
            // a metre of ground -- one station, which is a corner. So the jets'
            // own stretches get a few passes of an ordinary smoothing filter
            // afterwards, which works in the direction the corner is actually
            // measured in. It is a no-op down the straight of a drop (a filter
            // does nothing to a line) and it costs the crest a couple of
            // centimetres, which is a shape that was already right.
            // Weighted by where the neighbours actually ARE, not by how many
            // there are. The jets' stations are a quarter of a metre apart and the
            // line's own are two metres, and an index-weighted filter across that
            // boundary averages a station next door with one three houses down --
            // which does not smooth the step, it invents a new one.
            for (int pass = 0; pass < 8; ++pass) {
                std::vector<float> sm = H2;
                for (int i = 1; i + 1 < m; ++i) {
                    if (!zone[i]) continue;
                    const float run = S[i + 1] - S[i - 1];
                    if (run < 1e-5f) continue;
                    const float lin = glm::mix(H2[i - 1], H2[i + 1],
                                               (S[i] - S[i - 1]) / run);
                    sm[i] = glm::mix(H2[i], lin, 0.5f);
                }
                H2.swap(sm);
            }
            // The descent guarantee is the whole promise of this module, and
            // neither a smooth maximum nor a filter is monotone on paper. Held
            // here, not hoped for.
            for (int i = 1; i < m; ++i) H2[i] = std::min(H2[i], H2[i - 1]);
            // ...and the airborne fraction is re-derived from where the water
            // ended up, not from where it was before the rounding. It is what
            // stops the carve building a ramp under a jet, so it has to describe
            // the profile that is actually going to be drawn.
            for (int i = 0; i < m; ++i)
                A2[i] = glm::clamp((H2[i] - R2[i]) / 0.6f, 0.0f, 1.0f);

            // The ground is re-asked rather than interpolated: a station inserted
            // halfway down a cliff face is nowhere near the average of the two it
            // sits between, and the carve under it reads this number.
            for (int i = 0; i < m; ++i)
                G2[i] = groundAt ? groundAt(P2[i].x, P2[i].y) : 0.0f;

            // A control point names its stretch by station index, so the indices
            // move with the stations: the first new station at or after the old
            // one, which always exists because the resample only ever ADDS.
            {
                int seg = 0;
                for (int& ps : c.ptSample) {
                    const float want = c.s[glm::clamp(ps, 0, n - 1)];
                    while (seg + 1 < m && S[seg] < want) ++seg;
                    ps = seg;
                }
            }

            P.swap(P2);
            H.swap(H2);
            B.swap(B2);
            c.ground.swap(G2);
            c.air.swap(A2);
            c.s.swap(S);
            n = m;
            c.wander.assign(n, 0.0f);
            for (int i = 0; i < n; ++i)
                c.wander[i] = wander(c.s[i], st.meanderLength, st.seed);
            // Which stations are a fall is now a question about the RESAMPLED
            // profile. The plunge scoop and the whitewater both read it, and the
            // old flags against the new stations would put the scoop under the
            // wrong stretch of river.
            fall.assign(n, 0);
            for (int i = 1; i < n; ++i) {
                const float ds = std::max(c.s[i] - c.s[i - 1], 1e-4f);
                if ((H[i - 1] - H[i]) / ds > st.fallSlope) fall[i] = 1;
            }
        }
    }

    // 7) Widths, the plunge scoop, and the surface line itself.
    // The line is sized HERE and not back at step 2, because step 6b may have
    // added stations to it since -- and a course whose line is one station short
    // of its own widths is a buffer overrun, not a short river.
    c.line.resize(n);
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
        // A window in METRES, not in stations. The fall resample leaves stations
        // a few centimetres apart, and a fixed +/-2 there measures the turn of a
        // stretch shorter than the noise in it -- which reports a hairpin on a
        // straight line and pinches the channel to nothing at every waterfall.
        int a = i, b = i;
        while (a > 0     && c.s[i] - c.s[a] < 4.0f) --a;
        while (b < n - 1 && c.s[b] - c.s[i] < 4.0f) ++b;
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
        // A jet in mid air is not held out by its banks any more: it necks in
        // as it accelerates, which is most of what makes a fall read as one
        // thing leaving a ledge rather than as a ribbon pinned to a cliff.
        const float air = i < static_cast<int>(c.air.size()) ? c.air[i] : 0.0f;
        const float hw = c.half[i] * 0.99f * (1.0f - 0.22f * air);
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
            // ...and w says whether this is water on a bed at all: see
            // Course::air. The shader draws a flying sheet as filaments and a
            // lying one as ripples, and nothing else in the vertex can tell it
            // which it has -- a curtain's normal alone cannot, because a steep
            // cascade still has its water firmly on the rock.
            v.paint    = glm::vec4(depthFactor(k, st, uu) * c.deep[i],
                                   c.white[i], hw, air);
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
    // Spaced along the WATER, not along the map. A fifty-metre fall covers two
    // metres of plan distance, so spacing them by `s` puts one emitter on the
    // whole of it -- and a waterfall that mists at one point is a waterfall
    // nobody believes. `sSurf` is the fall's own length, which is what the mist
    // is actually spread over.
    float next = -1e9f;
    for (int i = 1; i < n; ++i) {
        const float air = i < static_cast<int>(c.air.size()) ? c.air[i] : 0.0f;
        if (c.white[i] < 0.55f || (c.slope[i] < st.rapidSlope && air < 0.2f))
            continue;
        if (c.sSurf[i] - next < spacing) continue;
        next = c.sSurf[i];
        SprayPoint sp;
        sp.pos      = c.line[i];
        sp.dir      = glm::normalize(glm::vec3(c.dir[i].x, -0.25f, c.dir[i].y));
        // Falling water throws far more of it than fast water does, and the
        // bottom of a drop throws the most of all -- so the strength follows the
        // gradient AND how long this water has been in the air.
        sp.strength = glm::clamp(c.white[i] * (0.4f + c.slope[i] * 1.4f + air),
                                 0.0f, 1.0f);
        sp.width    = c.half[i] * (1.6f + air);
        out.push_back(sp);
    }
    return out;
}

} // namespace rivergen
