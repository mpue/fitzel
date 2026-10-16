#version 330 core

in vec4  vClip;
in vec3  vWorldPos;
in vec2  vRestXZ;
in float vCrest;
in float vCalm;
in float vBreak;
in float vSurf;
in vec2  vShoreSlope;
in float vShorePhase;
in float vColumn;
out vec4 FragColor;

// The ocean's cascades (see water.vert): slopes for the normal, displacement
// for the foam the folding crests left (w).
uniform sampler2D uSlope0;
uniform sampler2D uSlope1;
uniform sampler2D uSlope2;
uniform sampler2D uDisp0;
uniform sampler2D uDisp1;
uniform vec3  uPatch;
uniform float uOceanN;
uniform sampler2D uWaveTex;     // the waves things make: height, slope x, slope z
uniform vec2  uWaveOrigin;
uniform float uWaveExtent;
uniform float uWhitecaps;       // how readily crests break into foam (0..2)
uniform float uWaveHs;          // significant wave height, m
uniform int   uFarPass;         // over the far terrain's lakes (see water.vert)
uniform float uOceanReach;      // the far pass ends here, where the far lakes take over

uniform sampler2D uReflection;
uniform sampler2D uRefraction;
uniform sampler2D uRefractionDepth; // scene depth behind the water
uniform float uNear;
uniform float uFar;
uniform int   uOrtho;      // 1: orthographic view -- the depth buffer is linear already
uniform float uFoamWidth; // world-ish depth over which shoreline foam fades
uniform float uRainRings; // drop impacts: strength (0 = off)
uniform float uRainDensity; // ...and how many land (0 = nothing falling)

uniform vec3  uCameraPos;
uniform vec3  uLightDir;     // towards the light
uniform vec3  uLightColor;
uniform vec3  uAmbient;      // sky/fill light (for lit foam)
uniform float uTime;
uniform vec3  uWaterColor;
uniform float uWaveStrength; // how far the waves bend what is seen through and in them
uniform float uReflectivity; // max mirror strength (Fresnel cap), 0..1
uniform float uClarity;      // water clarity: higher = clearer (less depth tint)
uniform float uIor;          // index of refraction (water ~1.33)

// Atmospheric fog (matches lit.frag).
uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

uniform float uExposure;
uniform int   uTonemap;

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 toOutput(vec3 c) {
    if (uTonemap == 1) {
        c = acesTonemap(c * uExposure);
        c = pow(c, vec3(1.0 / 2.2));
    }
    return c;
}

// --- Value-noise fBm (for ripples) -----------------------------------------
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i),            hash21(i + vec2(1, 0)), u.x),
               mix(hash21(i + vec2(0,1)), hash21(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return s;
}

// Window-space depth [0,1] -> linear eye distance.
float linearDepth(float d) {
    float z = d * 2.0 - 1.0;
    if (uOrtho == 1) return uNear + d * (uFar - uNear);
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

// One cascade's slope at the rest position, with the hardware picking the mip:
// far off, the waves finer than a pixel average away -- which is right, they
// are no longer seen as waves -- and what they did to the light becomes the
// roughness below.
vec2 slopeOf(sampler2D t, float L) {
    return texture(t, vRestXZ / L + 0.5 / uOceanN).xy;
}

// GGX, the microfacet distribution every physically based renderer uses: the
// share of the surface's tiny facets turned exactly toward the half vector.
float ggx(float NdH, float a) {
    float a2 = a * a;
    float d  = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}

// Rain on the lake: the drops close to, the stipple further out, the matt sheen
// beyond -- shared with river.frag, so one shower falls on both the same way.
#include "rainwater.glsl"
#include "fishrings.glsl"

// Where this surface ends (xz min, xz max). With the horizon terrain on, past
// the streamed ring the far terrain draws its own lakes -- and this quad, drawn
// after the far terrain's depth was cleared, would lie over its hills.
uniform vec4 uWaterClip;

void main() {
    // The near pass draws the streamed square, the far pass the rest out to
    // its reach -- the same surface in two depth ranges, meeting at the edge.
    // By the rest position, so a wave leaning over the line does not tear it.
    bool inside = vRestXZ.x >= uWaterClip.x && vRestXZ.y >= uWaterClip.y &&
                  vRestXZ.x <= uWaterClip.z && vRestXZ.y <= uWaterClip.w;
    if ((uFarPass == 0) != inside) discard;
    if (uFarPass == 1 && distance(vRestXZ, uCameraPos.xz) > uOceanReach) discard;
    // Projective UVs from clip-space position. The reflection is rendered with a
    // mirror matrix (view * scale(1,-1,1)), so the texture is already correctly
    // oriented -- both targets sample at the fragment's own screen UV.
    vec2 ndc = (vClip.xy / vClip.w) * 0.5 + 0.5;

    // The surface normal: the three cascades' slopes summed, calmed in the
    // shallows like the displacement was, plus the waves things made.
    // Shallow water keeps its ripples: only the height of the waves is held
    // down there (water.vert), and a shoal turned into a mirror reads as a hole.
    // The two smaller patches repeat every hundred metres or so, and from
    // high up a repeat is a grid. So their share varies with a large, slow,
    // never-repeating field -- the patches of rougher and smoother water a
    // gusting wind leaves, which the sea has anyway.
    float gusty = 0.55 + 0.9 * fbm(vRestXZ * 0.0035 + vec2(uTime * 0.012, -uTime * 0.007));
    vec2 slope = (slopeOf(uSlope0, uPatch.x) +
                  (slopeOf(uSlope1, uPatch.y) + slopeOf(uSlope2, uPatch.z)) * gusty) *
                 mix(0.7, 1.0, vCalm);
    vec2 wuv = (vRestXZ - uWaveOrigin) / uWaveExtent;
    float wedge = smoothstep(0.0, 0.08, min(min(wuv.x, wuv.y), min(1.0 - wuv.x, 1.0 - wuv.y)));
    if (wedge > 0.0) slope += texture(uWaveTex, wuv).yz * wedge;
    slope += vShoreSlope;
    vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
    // How much of the water one pixel covers: the waves averaged away in it
    // are still tilting the light, so they come back as roughness.
    float px = max(length(dFdx(vRestXZ)), length(dFdy(vRestXZ)));
    // Rain striking the lake. Before the Fresnel and the refraction offset below,
    // because a drop ring is only visible in what it does to those -- the surface
    // never moves, so if the rings do not bend the reflection they do nothing.
    // And not only the rings: from further off than a ring can be drawn the rain
    // is still there, as a stipple and then as a matt sheen (rainwater.glsl).
    float rainRough = 0.0, splash = 0.0;
    if (uRainRings > 0.002 && uRainDensity > 0.002) {
        vec3 dpx = dFdx(vWorldPos), dpy = dFdy(vWorldPos);
        float wpx = max(length(dpx.xz), length(dpy.xz));
        N = rainRings(N, vWorldPos.xz, uRainRings, uRainDensity, wpx, uTime, splash);
        N = rainDimples(N, vWorldPos.xz, uRainRings, uRainDensity, wpx, uTime);
        rainRough = rainRoughness(vWorldPos.xz, uRainRings * uRainDensity, uTime);
    }
    if (uFishRingCount > 0) {
        vec3 dpx = dFdx(vWorldPos), dpy = dFdy(vWorldPos);
        N = fishRings(N, vWorldPos.xz, min(length(dpx.xz), length(dpy.xz)), 1.0);
    }
    vec3  V = normalize(uCameraPos - vWorldPos);

    // Water column thickness = scene depth behind the water minus the surface
    // depth. Sampled at the *undistorted* screen position so it tracks the real
    // shoreline. `shallow` runs 0 at the waterline -> 1 in open water.
    float sceneZ = linearDepth(texture(uRefractionDepth, ndc).r);
    float waterZ = linearDepth(gl_FragCoord.z);
    // In the far pass the refraction depth belongs to another depth range:
    // that water is the open lake, deep.
    float thickness = uFarPass == 1 ? 1000.0 : max(sceneZ - waterZ, 0.0);
    float shallow   = smoothstep(0.0, 1.2, thickness);

    // Ripple distortion, damped in the shallows so the near-shore bed stays
    // readable (no smeared, hard edge). The apparent refraction scales with the
    // air->water index contrast, normalised so water's 1.33 matches the tuned look.
    float bend = (uIor - 1.0) / 0.33;
    vec2 distortion = N.xz * uWaveStrength * shallow * bend;
    vec2 refractUV  = clamp(ndc + distortion, 0.002, 0.998);
    // A tilted mirror turns what it reflects by twice its tilt, and a far
    // shore seen in it moves across the screen by about that much -- far more
    // than the bed seen through it does. Broken reflections are most of what
    // makes water look like water rather than glass.
    vec2 reflectUV  = clamp(ndc + N.xz * uWaveStrength * 2.0 * mix(0.35, 1.0, shallow), 0.002, 0.998);

    // ...and every pixel of it holds many small facets tilted every which way,
    // so what it shows is their average: the reflection blurs with distance and
    // with how rough the sea is. Read from the reflection's mips (main builds
    // them) -- without this a far shore or a cloudy sky came back from the
    // waves as a hard camouflage of light and dark.
    float glossLod = clamp(log2(1.0 + px * 2.0) + 6.0 * length(slope) * smoothstep(0.02, 0.5, px), 0.0, 6.0);
    vec3 reflectCol = textureLod(uReflection, reflectUV, glossLod).rgb;
    vec3 refractCol = texture(uRefraction, refractUV).rgb;
    // A rain-roughened surface mirrors nothing sharply: thousands of drops a
    // second each tilt it a little, and what reaches the eye is the average. A
    // ring of taps around the sample, as wide as the rain is rough.
    if (rainRough > 0.01) {
        float rad = rainRough * 0.007;
        vec3 rs = reflectCol, ts = refractCol;
        for (int i = 0; i < 6; ++i) {
            float a = float(i) * 1.0471976 + 0.4;
            vec2  o = vec2(cos(a), sin(a) * 1.78) * rad;   // round on screen, not in UV
            rs += texture(uReflection, clamp(reflectUV + o, 0.002, 0.998)).rgb;
            ts += texture(uRefraction, clamp(refractUV + o, 0.002, 0.998)).rgb;
        }
        reflectCol = rs / 7.0;
        refractCol = ts / 7.0;
    }

    // Beer-Lambert depth absorption: clear over a shallow bed, tinting toward the
    // water colour with depth. Red is absorbed fastest -> natural blue-green.
    // `uClarity` scales the extinction (higher = clearer water, less tint).
    vec3  absorb = vec3(0.30, 0.14, 0.10) / max(uClarity, 0.05);
    vec3  trans  = exp(-absorb * thickness);
    vec3  deep   = pow(uWaterColor, vec3(2.2));
    vec3  body   = mix(deep, refractCol, trans); // shallow -> bed, deep -> tint

    // Schlick Fresnel with the reflectance-at-normal-incidence derived from the
    // index of refraction: F0 = ((1-n)/(1+n))^2  (~0.02 for water's 1.33). Never a
    // perfect mirror (capped), and reduced over shallow water where you'd see the
    // bed, not the sky -- kills the oily, glass-like look.
    float cosT = clamp(dot(V, N), 0.0, 1.0);
    float f0 = (1.0 - uIor) / (1.0 + uIor);
    f0 *= f0;
    float F = f0 + (1.0 - f0) * pow(1.0 - cosT, 5.0);
    F = min(F, uReflectivity) * mix(0.35, 1.0, shallow);
    // ...and a matt surface reflects less of the sky as a mirror and more of it
    // as a soft grey sheen: the look of a lake while it rains.
    F *= 1.0 - 0.2 * rainRough;

    vec3 color = mix(body, reflectCol, F);
    color = mix(color, uAmbient * 0.85, 0.06 * rainRough);

    // Light through the waves. Water is not opaque: sun entering the back of a
    // crest scatters out of its thin top toward an eye looking into the light,
    // which is the green glow in a wave about to break -- and a little of it
    // comes out of every face turned toward the viewer.
    vec3  L = normalize(uLightDir);
    vec3  sssCol = pow(uWaterColor, vec3(2.2)) * vec3(1.6, 3.2, 2.6) + vec3(0.0, 0.035, 0.03);
    float into   = pow(max(dot(L, -V), 0.0), 4.0) * pow(clamp(0.5 - 0.5 * dot(L, N), 0.0, 1.0), 3.0);
    float sss    = 4.0 * max(vCrest, 0.0) * into + 0.18 * pow(max(dot(V, N), 0.0), 2.0);
    color += sssCol * uLightColor * sss * (1.0 - F) * vCalm;

    // The sun on the water. GGX rather than a fixed power: up close a hard,
    // tight glint; far off the waves the pixel averaged away widen it into the
    // long, broken road of light toward the sun -- and the rain dulls it.
    vec3  H = normalize(L + V);
    float rough = clamp(0.05 + 0.08 * log2(1.0 + px * 5.0), 0.05, 0.4);
    rough = mix(rough, 0.45, rainRough * 0.6);
    float NdL = max(dot(N, L), 0.0);
    float LdH = max(dot(L, H), 0.05);
    float Fs  = f0 + (1.0 - f0) * pow(1.0 - LdH, 5.0);
    float spec = ggx(max(dot(N, H), 0.0), rough * rough) * Fs * 0.25 / (LdH * LdH);
    color += uLightColor * min(spec * NdL, 40.0);
    // The crowns the drops throw up, close to: lit spray, not a glint.
    color += (uAmbient * 1.4 + uLightColor * max(L.y, 0.0) * 0.6) * min(splash, 1.0);

    // Shoreline foam: a thin, soft band where the water is shallow (no hard line).
    // Measured along the view without a shore map -- which on a gentle beach
    // is a wide strip -- and with one by the water actually standing on the
    // ground: the lacy edge of the sheet running up the sand.
    float shoreF = vColumn >= 0.0
                 ? 1.0 - smoothstep(0.0, 0.08 * uFoamWidth, vColumn)
                 : 1.0 - smoothstep(0.0, uFoamWidth, thickness);
    float fn     = fbm(vWorldPos.xz * 0.6 + vec2(uTime * 0.08, -uTime * 0.05));
    shoreF       = smoothstep(0.30, 0.85, shoreF) * (0.4 + 0.6 * fn);

    // Whitecaps: foam where the crests folded over (the ocean marks it per
    // cascade as it happens and lets it fade), given a texture of bubbles and
    // gaps so it reads as foam rather than as a white stain. COVERAGE, not
    // brightness: thin foam is a lace with water between, thick foam a sheet.
    // The middle patch's foam where the big swells are cresting and the wind
    // is gusting: that is where a sea breaks -- and the big patch does not
    // repeat within sight, so neither does the foam.
    float folded = (texture(uDisp0, vRestXZ / uPatch.x + 0.5 / uOceanN).w * 0.8 +
                    texture(uDisp1, vRestXZ / uPatch.y + 0.5 / uOceanN).w *
                        smoothstep(-0.3, 0.9, vCrest) * gusty) * uWhitecaps * vCalm;
    float lace = 0.55 * fbm(vRestXZ * 1.7 + vec2(uTime * 0.05, 0.0)) +
                 0.45 * fbm(vRestXZ * 5.3 - vec2(0.0, uTime * 0.08));
    float whitecap = smoothstep(0.0, 0.25, clamp(folded, 0.0, 1.2) - (1.0 - lace) * 0.9);
    // Far off, a pixel holds many patches of foam and the gaps between them:
    // what it shows is how much of it is white, not the lace.
    whitecap = mix(whitecap, clamp(folded * 0.8, 0.0, 1.0), smoothstep(0.4, 4.0, px));
    // Surf: where the bed comes up under a big sea, the waves break.
    whitecap = max(whitecap, (1.0 - vCalm) * smoothstep(0.4, 2.5, uWaveHs) *
                             smoothstep(0.35, 0.65, lace));
    // The breakers: a white roll along the crest that is tipping over, and
    // behind it the surf zone's churned water -- a lace of foam that thickens
    // toward the beach and drains in streaks.
    float roll  = smoothstep(0.15, 0.6, vBreak + (lace - 0.5) * 0.4);
    // Each broken wave runs in as a line of white water and leaves the water
    // behind it streaked and fading -- so the surf is BANDS, one per breaker,
    // with green water between them, not one white strip along the beach.
    // `vShorePhase` runs the wrong way for "behind": the crest passed at 0
    // and the next arrives at 1, so the freshest foam is just above 1.
    float behind = 1.0 - vShorePhase;
    float bore   = exp(-behind * 4.5) * smoothstep(0.0, 0.04, behind);
    float churn  = vSurf * smoothstep(0.55, 0.85, lace * (0.35 + 0.9 * bore) + bore * 0.45);
    whitecap = max(whitecap, max(roll, churn));

    float foam = clamp(max(shoreF, whitecap * 0.85), 0.0, 1.0);

    // Foam is a lit surface (responds to the sun, dims at night), kept softer so
    // it reads as spray rather than a bright decal.
    vec3 foamColor = uAmbient * 1.1 + uLightColor * max(dot(N, L), 0.0) * 0.8;
    color = mix(color, foamColor, foam * 0.7);

    // Atmospheric fog so distant water blends into the horizon haze.
    vec3  toFrag = vWorldPos - uCameraPos;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(uCameraPos.y - uFogHeight) * b);
    float od = (abs(rd.y) > 1e-4)
             ? c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y)
             : c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));
    float sunAmt = pow(max(dot(rd, normalize(uLightDir)), 0.0), 4.0);
    color = mix(color, mix(uFogColor, uFogSunColor, sunAmt), clamp(fog, 0.0, 1.0));

    FragColor = vec4(toOutput(color), 1.0);
}
