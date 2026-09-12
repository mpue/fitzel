#version 330 core

// Temporal anti-aliasing: the resolve.
//
// Every frame is rendered with the projection nudged by a different sub-pixel
// offset (a Halton sequence -- see PostChain::jitter), so over a handful of
// frames every pixel has been sampled at many points inside its footprint. This
// pass keeps a running average of those samples in a history buffer, following
// each surface as it moves across the screen. What FXAA could never do -- a
// grass blade thinner than a pixel, a fence wire, the far edge of a road --
// stops crawling, because it is no longer one sample deciding a whole pixel.
//
// Following the surface is the hard part:
//  - Anything that moved on its own (a car, a door) has a measured motion
//    vector (Renderer::renderMotion, alpha 1).
//  - Everything else is reprojected from depth and the two cameras, which is
//    exact for a world that stands still. The depth used is the NEAREST of the
//    3x3 neighbourhood, so an object's edge takes the object's motion instead
//    of the background's -- the usual cause of a ghosted outline.
//  - History that no longer belongs to the pixel (disocclusion, lighting that
//    changed, something transparent sliding past) is clipped into the colour
//    range the current frame's neighbourhood allows, in YCoCg, towards its
//    mean. That is what stops ghosting without a single special case.
// Colours are weighted by 1/(1+max) on the way in and unweighted on the way
// out (Karis), so one fireflying sun-glint cannot dominate an average.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uCur;       // this frame, jittered, linear HDR
uniform sampler2D uHistory;   // last frame's resolve
uniform sampler2D uDepth;     // this frame's depth
uniform sampler2D uMotion;    // measured motion for surfaces that moved (a = 1)
uniform int   uHasMotion;     // 0 = no motion pass ran this frame: depth only
uniform mat4  uInvCurVP;      // this frame's view-projection, unjittered, inverted
uniform mat4  uPrevVP;        // last frame's, unjittered
uniform vec2  uTexel;
uniform vec2  uJitter;        // this frame's offset in UV (to sample the unjittered point)
uniform float uBlend;         // share of the current frame at rest (0.1 = ~10 frames)
uniform int   uReset;         // 1 = there is no usable history this frame

vec3 weigh(vec3 c)   { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 unweigh(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

vec3 toYCoCg(vec3 c) {
    return vec3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                 0.5  * c.r             - 0.5  * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 fromYCoCg(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

vec3 fetchCur(vec2 uv) {
    vec3 c = textureLod(uCur, uv, 0.0).rgb;
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    return toYCoCg(weigh(max(c, vec3(0.0))));
}

// History through a 5-tap Catmull-Rom (Jimenez): bilinear softens the image a
// little more every frame it is resampled, and ten frames of that is a blur.
vec3 fetchHistory(vec2 uv) {
    vec2 size = 1.0 / uTexel;
    vec2 pos  = uv * size;
    vec2 c1   = floor(pos - 0.5) + 0.5;
    vec2 f    = pos - c1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc12 = (c1 + w2 / w12) * uTexel;
    vec2 tc0  = (c1 - 1.0) * uTexel;
    vec2 tc3  = (c1 + 2.0) * uTexel;
    vec3 r =
        textureLod(uHistory, vec2(tc12.x, tc0.y),  0.0).rgb * (w12.x * w0.y) +
        textureLod(uHistory, vec2(tc0.x,  tc12.y), 0.0).rgb * (w0.x  * w12.y) +
        textureLod(uHistory, vec2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y) +
        textureLod(uHistory, vec2(tc3.x,  tc12.y), 0.0).rgb * (w3.x  * w12.y) +
        textureLod(uHistory, vec2(tc12.x, tc3.y),  0.0).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    r /= max(wsum, 1e-4);
    if (any(isnan(r)) || any(isinf(r))) r = vec3(0.0);
    return toYCoCg(weigh(max(r, vec3(0.0))));
}

// Pull `h` towards the box centre until it lies inside it (Playdead).
vec3 clipToBox(vec3 lo, vec3 hi, vec3 h) {
    vec3 c = 0.5 * (hi + lo);
    vec3 e = 0.5 * (hi - lo) + 1e-5;
    vec3 v = h - c;
    vec3 a = abs(v / e);
    float m = max(a.x, max(a.y, a.z));
    return (m > 1.0) ? c + v / m : h;
}

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;

    // Neighbourhood statistics and the nearest depth around the pixel.
    vec3  m1 = vec3(0.0), m2 = vec3(0.0);
    vec3  centre = vec3(0.0);
    float nearest = 1.0;
    vec2  nearestUV = uv;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 suv = uv + vec2(x, y) * uTexel;
            vec3 s = fetchCur(suv);
            if (x == 0 && y == 0) centre = s;
            m1 += s;
            m2 += s * s;
            float d = textureLod(uDepth, suv, 0.0).r;
            if (d < nearest) { nearest = d; nearestUV = suv; }
        }
    }

    // Where this surface was last frame.
    vec2 motion;
    vec4 measured = (uHasMotion == 1) ? textureLod(uMotion, nearestUV, 0.0) : vec4(0.0);
    // a = 2 is not a measurement but a flag: something small and quick was
    // drawn here (a mote of pollen, Motes.hpp) that no history can follow --
    // show this frame's, or the resolve averages it away to nothing.
    float reactive = (uHasMotion == 1 && textureLod(uMotion, uv, 0.0).a > 1.5) ? 1.0 : 0.0;
    if (measured.a > 0.5 && measured.a < 1.5) {
        motion = measured.xy;
    } else {
        // Undo the jitter first: the depth was written at the jittered point,
        // the cameras are compared unjittered.
        vec4 ndc   = vec4((nearestUV - uJitter) * 2.0 - 1.0, nearest * 2.0 - 1.0, 1.0);
        vec4 world = uInvCurVP * ndc;
        world /= world.w;
        vec4 prev  = uPrevVP * world;
        vec2 prevUV = prev.xy / prev.w * 0.5 + 0.5;
        motion = (nearestUV - uJitter) - prevUV;
    }
    vec2 histUV = uv - motion;

    bool offscreen = any(lessThan(histUV, vec2(0.0))) || any(greaterThan(histUV, vec2(1.0)));
    if (uReset == 1 || offscreen) {
        FragColor = vec4(unweigh(fromYCoCg(centre)), 1.0);
        return;
    }

    // Variance box (mean +- 1.25 sigma): tighter than min/max, so a history
    // that is merely plausible for the neighbourhood does not survive.
    vec3 mean  = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 lo = mean - 1.25 * sigma;
    vec3 hi = mean + 1.25 * sigma;

    vec3 hist = clipToBox(lo, hi, fetchHistory(histUV));

    // Trust the history less the faster the pixel moves: in motion every
    // frame of it has been resampled more, and the eye forgives aliasing there
    // far more than it forgives smear.
    float speed = length(motion / uTexel);
    float blend = mix(uBlend, 0.4, clamp(speed / 30.0, 0.0, 1.0));
    blend = max(blend, 0.75 * reactive);
    vec3  res   = mix(hist, centre, blend);

    vec3 outc = unweigh(fromYCoCg(res));
    if (any(isnan(outc)) || any(isinf(outc))) outc = vec3(0.0);
    FragColor = vec4(min(outc, vec3(50000.0)), 1.0);
}
