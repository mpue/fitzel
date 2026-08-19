#version 330 core

// Volumetric fog: a BOX of participating medium, raymarched against the scene's
// depth buffer.
//
// This is not the atmospheric fog in lit.frag. That one is a per-pixel closed
// form -- exponential height haze over the whole world, no shape, no shadows,
// and no way to say "the mist sits in THIS valley". This is the other kind: a
// placed volume with an inside and an outside, whose density comes out of an
// animated 3D noise field, and which is marched step by step so the sun can be
// occluded along the way. That is what buys the two things the closed form can
// never have -- structure (banks, wisps, holes drifting through) and shafts.
//
// Output is NOT a finished image: rgb is the light scattered toward the eye and
// a is how much of the scene behind survives. The caller blends it over the HDR
// buffer as src + dst*srcAlpha, which is exactly the integration this march did.
//
// Runs at a fraction of the pane resolution (see VolumetricFog::Settings) and is
// tent-filtered on the way back up. Fog is low-frequency by nature, so the
// resolution worth paying for here is far below the one the scene needs.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D      uDepth;      // the scene depth, to stop the march
uniform sampler3D      uNoise;      // RGBA: R/G/B value-fBm bands, A worley
uniform sampler2DArray uShadowMap;  // the sun cascades, for the shafts

uniform mat4  uInvViewProj;
uniform vec3  uCamPos;
uniform vec3  uCamFwd;   // for the view depth a cascade is picked by
uniform float uTime;

// --- The volume ------------------------------------------------------------
uniform vec3  uBoxMin;
uniform vec3  uBoxMax;
uniform float uEdge;          // fraction of each half-extent that fades to nothing
uniform float uHeightFalloff; // how fast the medium thins toward the box top

// --- The medium ------------------------------------------------------------
uniform float uDensity;       // extinction per metre where the noise is solid
uniform vec3  uColor;         // the fog albedo/tint
uniform float uCoverage;      // noise level below which there is no fog at all

// --- The noise -------------------------------------------------------------
uniform float uNoiseScale;    // world metres -> noise space
uniform float uDetail;        // how hard the worley band breaks the shape up
uniform float uWarp;          // domain warp: the swirl in the banks
uniform vec3  uWind;          // metres per second the field drifts

// --- Lighting --------------------------------------------------------------
uniform vec3  uSunDir;        // points *towards* the sun
uniform vec3  uSunColor;
uniform vec3  uAmbient;
uniform float uG;             // Henyey-Greenstein anisotropy
uniform float uSunIntensity;
uniform float uAmbientIntensity;
uniform int   uSelfShadow;    // short march toward the sun (depth inside a bank)
uniform float uLightStep;     // its step length, in metres

#define MAX_CASCADES 4
uniform mat4  uLightSpace[MAX_CASCADES];
uniform float uCascadeSplits[MAX_CASCADES];
uniform int   uCascadeCount;
uniform int   uShafts;        // sample the cascades at all?

uniform int   uSteps;

const float PI = 3.14159265;

// A direction with no exact zero in it, so the slab test below can divide by it.
vec3 safeDir(vec3 d) {
    return vec3(abs(d.x) < 1e-6 ? 1e-6 : d.x,
                abs(d.y) < 1e-6 ? 1e-6 : d.y,
                abs(d.z) < 1e-6 ? 1e-6 : d.z);
}

float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// Interleaved gradient noise -- the per-pixel offset of the first sample. Without
// it a fixed step lands on the same planes for every pixel and the march shows as
// concentric shells; with it the error becomes a fine dither that the upsample
// tent filter resolves.
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// How much of the box a point is inside: 1 in the middle, easing to 0 across the
// outer `uEdge` of every half-extent, times the vertical thinning. Multiplying
// the three axes is what keeps the corners from showing a seam.
float shell(vec3 p) {
    vec3 c  = (uBoxMin + uBoxMax) * 0.5;
    vec3 hs = (uBoxMax - uBoxMin) * 0.5;
    vec3 t  = clamp((hs - abs(p - c)) / max(hs * uEdge, vec3(1e-3)), 0.0, 1.0);
    float e = t.x * t.y * t.z;
    e = e * e * (3.0 - 2.0 * e);
    float hRel = clamp((p.y - uBoxMin.y) / max(uBoxMax.y - uBoxMin.y, 1e-3), 0.0, 1.0);
    return e * exp(-uHeightFalloff * 4.0 * hRel);
}

// The full medium at a point: three fetches -- a low-frequency vector field that
// warps the lookup (the swirl), the shape band it warps, and a worley band that
// breaks that shape into billows.
float density(vec3 p) {
    float s = shell(p);
    if (s <= 0.002) return 0.0;
    vec3 q = (p - uWind * uTime) * uNoiseScale;
    vec3 w = (texture(uNoise, q * 0.30).rgb - 0.5) * uWarp;
    float base  = texture(uNoise, q + w).r;
    float shape = smoothstep(uCoverage, uCoverage + 0.32, base);
    float det   = texture(uNoise, q * 3.1 + w).a;
    // The worley band SCALES the shape rather than being subtracted from it.
    // Subtracting is the cloud recipe, and it is wrong here: a cloud's core sits
    // near 1, so only its fringe is low enough for the subtraction to bite. Fog
    // is a low, wide field whose typical value is already small -- subtract a
    // detail band from that and the whole volume clears, which is what the first
    // version of this did (an empty scene, and every knob in it doing nothing).
    // Scaling keeps the bank and breaks it up instead.
    float f = shape * mix(1.0, det, uDetail);
    return uDensity * f * s;
}

// The same field with the detail bands left out -- one fetch. Used only by the
// light march, where the difference is invisible and the cost is threefold.
float coarseDensity(vec3 p) {
    float s = shell(p);
    if (s <= 0.002) return 0.0;
    vec3 q = (p - uWind * uTime) * uNoiseScale;
    float base = texture(uNoise, q).r;
    return uDensity * smoothstep(uCoverage, uCoverage + 0.32, base) * s;
}

// How much sun reaches a point through the fog itself. Three taps toward the
// sun: enough to darken the inside of a bank against its lit rim, which is the
// whole reason to bother -- an evenly lit volume looks like coloured glass.
float selfShadow(vec3 p) {
    float sum = 0.0;
    for (int j = 1; j <= 3; ++j)
        sum += coarseDensity(p + uSunDir * (uLightStep * float(j)));
    return exp(-sum * uLightStep);
}

int cascadeFor(float viewDepth) {
    for (int i = 0; i < uCascadeCount; ++i) {
        if (viewDepth < uCascadeSplits[i]) return i;
    }
    return max(uCascadeCount - 1, 0);
}

// Is this point in the sun, according to the same cascades the surfaces use?
// One tap, no PCF: the march averages dozens of them along the ray and the start
// offset is dithered per pixel, so the filtering happens for free -- a 5x5 kernel
// here would buy a slower shaft with the same edge.
float sunVisibility(vec3 p) {
    if (uShafts == 0 || uCascadeCount == 0) return 1.0;
    int l = cascadeFor(dot(p - uCamPos, uCamFwd));
    vec4 ls = uLightSpace[l] * vec4(p, 1.0);
    vec3 pr = ls.xyz / ls.w * 0.5 + 0.5;
    if (pr.z > 1.0 || any(lessThan(pr.xy, vec2(0.0))) ||
        any(greaterThan(pr.xy, vec2(1.0))))
        return 1.0;   // outside the cascade: unshadowed rather than black
    float closest = texture(uShadowMap, vec3(pr.xy, float(l))).r;
    return (pr.z - 0.0015 > closest) ? 0.0 : 1.0;
}

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;

    // The view ray, and how far along it the scene stands. The eye, the
    // near-plane point and the far-plane point are collinear under a perspective
    // projection, so the ray can start at the eye and the depth buffer gives its
    // end.
    vec4 farH = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 rd   = normalize(farH.xyz / farH.w - uCamPos);

    float d = texture(uDepth, uv).r;
    float tScene = 1.0e9;                    // sky: nothing stops the march
    if (d < 1.0) {
        vec4 h = uInvViewProj * vec4(vNdc, d * 2.0 - 1.0, 1.0);
        tScene = length(h.xyz / h.w - uCamPos);
    }

    // Slab test against the box: where the ray enters and leaves the volume.
    vec3 inv = 1.0 / safeDir(rd);
    vec3 a = (uBoxMin - uCamPos) * inv;
    vec3 b = (uBoxMax - uCamPos) * inv;
    vec3 lo = min(a, b), hi = max(a, b);
    float t0 = max(max(max(lo.x, lo.y), lo.z), 0.0);
    float t1 = min(min(min(hi.x, hi.y), hi.z), tScene);
    if (t1 <= t0) { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    int   steps = clamp(uSteps, 8, 128);
    float dt    = (t1 - t0) / float(steps);
    float t     = t0 + dt * ign(gl_FragCoord.xy);

    float phase   = phaseHG(dot(rd, uSunDir), uG);
    // No sun in-scatter once the sun is down; the ambient term carries the night.
    float sunUp   = smoothstep(-0.12, 0.15, uSunDir.y);
    vec3  ambient = uAmbient * uAmbientIntensity;

    vec3  scatter = vec3(0.0);
    float T = 1.0;
    for (int i = 0; i < steps; ++i) {
        vec3 p = uCamPos + rd * t;
        float dens = density(p);
        if (dens > 0.0005) {
            float ext = dens * dt;
            float lit = sunVisibility(p) * (uSelfShadow == 1 ? selfShadow(p) : 1.0);
            vec3  L = (uSunColor * (uSunIntensity * phase * sunUp * lit) + ambient) * uColor;
            // Analytic integration of one segment: what this slab scatters is
            // tied to what it absorbs, so the two can never disagree and the fog
            // stays energy-conserving however coarse the steps are.
            scatter += T * L * (1.0 - exp(-ext));
            T *= exp(-ext);
            if (T < 0.004) break;   // saturated: the rest is invisible
        }
        t += dt;
    }

    FragColor = vec4(scatter, T);
}
