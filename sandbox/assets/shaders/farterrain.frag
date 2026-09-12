#version 330 core

// The far terrain's surface (see FarTerrain.hpp). At these distances a terrain
// layer texture is one averaged colour, so none is sampled: what the eye reads
// at five kilometres is where the forest stops, where rock breaks through and
// where the snow lies -- decided here from height, slope and moisture -- lit by
// the sun with the ranges shadowing each other, and seen through the air.

in vec3  vWorldPos;
in float vSunk;
out vec4 FragColor;

uniform int   uCutHole;          // 1 = drop the sunk part (the water mirror)

uniform sampler2D uHeight;       // this ring: r = height, g = moisture
uniform sampler2D uCoarse;       // the next ring out (for long shadows)
uniform vec2  uOrigin;
uniform float uSize;
uniform vec2  uCoarseOrigin;
uniform float uCoarseSize;
uniform float uGrid;
uniform float uCell;

uniform vec3  uCamPos;
uniform vec3  uLightDir;         // towards the sun
uniform vec3  uLightColor;
uniform vec3  uAmbient;
uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;
uniform float uTime;

uniform float uSnowLevel;
uniform float uTreeLine;
uniform float uWaterLevel;
uniform vec3  uGrassTint;
uniform vec3  uCanopy;           // the forest's mean foliage colour (linear)

#include "meadow.glsl"
#include "ecology.glsl"

// --- Noise ------------------------------------------------------------------
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1, 0));
    float c = hash21(i + vec2(0, 1)), d = hash21(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// fBm that fades each octave out once it is finer than the pixel it lands on
// (`px` = world metres per pixel). Without a mip chain, procedural detail on a
// surface kilometres away is exactly the shimmer it is supposed to hide.
float fbmPx(vec2 p, float scale, float px) {
    float s = 0.0, a = 0.5, f = 1.0 / scale;
    for (int i = 0; i < 5; ++i) {
        float fade = 1.0 - smoothstep(0.25, 0.9, px * f);
        s += a * (vnoise(p * f) - 0.5) * fade;
        f *= 2.07; a *= 0.5;
    }
    return s;   // about -0.5..0.5
}

// --- The heightfields -------------------------------------------------------
vec2 ringUv(vec2 xz, vec2 origin, float size) {
    float cell = size / (uGrid - 1.0);
    return ((xz - origin) / cell + 0.5) / uGrid;
}
float groundAt(vec2 xz) {
    vec2 uv = ringUv(xz, uOrigin, uSize);
    if (all(greaterThan(uv, vec2(0.002))) && all(lessThan(uv, vec2(0.998))))
        return textureLod(uHeight, uv, 0.0).r;
    vec2 cv = ringUv(xz, uCoarseOrigin, uCoarseSize);
    return textureLod(uCoarse, clamp(cv, 0.0, 1.0), 0.0).r;
}

// The ranges shadow each other: march the heightfield towards the sun with
// steps that grow as they go, keeping the closest the ray came to the ground
// (relative to how far it had travelled) -- which is also the penumbra.
float terrainShadow(vec3 p, vec3 n, vec3 L) {
    if (L.y <= 0.0) return 0.0;
    float res = 1.0;
    float s   = uCell * 1.5;
    vec3  o   = p + n * (uCell * 0.6);
    for (int i = 0; i < 28; ++i) {
        vec3  q = o + L * s;
        float d = q.y - groundAt(q.xz);
        res = min(res, 10.0 * d / s);
        if (res < -0.05 || q.y > 4500.0) break;
        s *= 1.25;
    }
    return smoothstep(0.0, 1.0, res);
}

// --- The air ----------------------------------------------------------------
// The sky's own horizon gradient (sky.frag's skyColor without disc and stars):
// the colour a range dissolves into has to be the colour of the sky behind it,
// or every silhouette carries a seam.
vec3 skyAir(vec3 dir) {
    float day = smoothstep(-0.12, 0.18, uLightDir.y);
    vec3 zen  = mix(vec3(0.01, 0.02, 0.06), vec3(0.20, 0.42, 0.80), day);
    vec3 hor  = mix(vec3(0.04, 0.06, 0.12), vec3(0.70, 0.82, 0.95), day);
    float h   = clamp(dir.y, 0.0, 1.0);
    vec3 col  = mix(hor, zen, pow(h, 0.5));
    float lowSun = (1.0 - smoothstep(0.0, 0.35, uLightDir.y)) * day;
    float toSun  = max(dot(normalize(vec3(dir.x, 0.0, dir.z) + 1e-5),
                           normalize(vec3(uLightDir.x, 0.0, uLightDir.z) + 1e-5)), 0.0);
    col += vec3(0.85, 0.35, 0.10) * lowSun * pow(toSun, 3.0) * (1.0 - h);
    return pow(col, vec3(2.2));
}

// Exponential height fog, exactly lit.frag's -- the streamed ground meets this
// one at the ring's edge, and the two must fog alike or the seam is a line.
vec3 applyFog(vec3 color, vec3 p, vec3 eye, vec3 L) {
    vec3  toFrag = p - eye;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(eye.y - uFogHeight) * b);
    float od = (abs(rd.y) > 1e-4) ? c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y)
                                  : c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));
    float sunAmt = pow(max(dot(rd, L), 0.0), 4.0);
    return mix(color, mix(uFogColor, uFogSunColor, sunAmt), clamp(fog, 0.0, 1.0));
}

// ...and the air itself, which the height fog (a ground haze with a scale
// height of tens of metres) knows nothing about: clean air is still air, and
// over twenty kilometres it turns a range blue and lifts its shadows. Thinner
// with altitude (scale height 1.6 km), so a summit stands out of the haze its
// own foot is lost in.
vec3 applyAir(vec3 color, vec3 p, vec3 eye, vec3 L) {
    vec3  toFrag = p - eye;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    const float H    = 1600.0;
    const float Lair = 17000.0;       // metres to 1/e in air at sea level
    float y0 = max(eye.y, 0.0), y1 = max(p.y, 0.0);
    float avgD = (abs(y1 - y0) > 1.0)
               ? H * (exp(-y0 / H) - exp(-y1 / H)) / (y1 - y0)
               : exp(-y0 / H);
    float a = 1.0 - exp(-dist / Lair * avgD);
    vec3 air = skyAir(normalize(vec3(rd.x, max(rd.y, 0.015), rd.z)));
    air += uFogSunColor * pow(max(dot(rd, L), 0.0), 6.0) * 0.30;   // forward scatter
    return mix(color, air, a);
}

vec3 lin(vec3 srgb) { return pow(srgb, vec3(2.2)); }

void main() {
    if (uCutHole == 1 && vSunk > 0.01) discard;
    vec3  P    = vWorldPos;
    vec2  xz   = P.xz;
    vec3  L    = normalize(uLightDir);
    vec2  uv   = ringUv(xz, uOrigin, uSize);
    vec2  hm   = texture(uHeight, uv).rg;
    float h    = hm.r;
    float moist = hm.g;
    float px   = length(fwidth(xz));                  // metres per pixel

    // The shading normal from the field itself (smooth, not the facets).
    float e  = uCell;
    float hl = texture(uHeight, ringUv(xz - vec2(e, 0.0), uOrigin, uSize)).r;
    float hr = texture(uHeight, ringUv(xz + vec2(e, 0.0), uOrigin, uSize)).r;
    float hd = texture(uHeight, ringUv(xz - vec2(0.0, e), uOrigin, uSize)).r;
    float hu = texture(uHeight, ringUv(xz + vec2(0.0, e), uOrigin, uSize)).r;
    vec3  N  = normalize(vec3(hl - hr, 2.0 * e, hd - hu));

    // Fine relief the grid is too coarse to carry: gullies and ribs as a bump
    // (surface-gradient method), strongest on the steep ground where it lives.
    float steep = 1.0 - N.y;
    float bump  = fbmPx(xz, 55.0, px) * (4.0 + 26.0 * steep);
    {
        vec3  dpx = dFdx(P), dpy = dFdy(P);
        vec3  r1 = cross(dpy, N), r2 = cross(N, dpx);
        float det = dot(dpx, r1);
        if (abs(det) > 1e-6) {
            vec3 g = (dFdx(bump) * r1 + dFdy(bump) * r2) / det;
            N = normalize(N - g * 0.9);
        }
    }
    float slope = degrees(acos(clamp(N.y, -1.0, 1.0)));

    vec3  V    = normalize(uCamPos - P);
    vec3  color;

    if (h < uWaterLevel - 0.05) {
        // --- Lakes and sea: a flat mirror of the sky, darker where deep ---
        vec3  Nw  = normalize(vec3(fbmPx(xz + uTime * 0.6, 9.0, px) * 0.05, 1.0,
                                   fbmPx(xz.yx - uTime * 0.5, 7.0, px) * 0.05));
        float cosV = max(dot(Nw, V), 0.0);
        float fres = 0.02 + 0.98 * pow(1.0 - cosV, 5.0);
        vec3  R    = reflect(-V, Nw);
        vec3  body = lin(vec3(0.05, 0.09, 0.10)) * (uAmbient + uLightColor * max(L.y, 0.0) * 0.15);
        vec3  refl = skyAir(normalize(vec3(R.x, max(R.y, 0.0), R.z)));
        float glint = pow(max(dot(R, L), 0.0), 400.0) * 6.0;
        color = mix(body, refl, fres) + uLightColor * glint * terrainShadow(P, vec3(0, 1, 0), L);
    } else {
        // --- What grows, what shows, what lies on it ---
        float n1 = fbmPx(xz, 190.0, px) + 0.5;          // 0..1, patches
        float n2 = fbmPx(xz + 71.0, 45.0, px) + 0.5;
        float n3 = fbmPx(xz - 233.0, 900.0, px) + 0.5;  // regions

        // The valley's meadow is the near field's own colour (meadow.glsl), so
        // the streamed ground and this one meet without a change of season.
        vec3 meadow  = pow(meadowColour(xz, 0.62, px), vec3(2.2)) * uGrassTint;
        vec3 alpine  = mix(lin(vec3(0.36, 0.38, 0.22)), lin(vec3(0.44, 0.40, 0.28)), n2);
        vec3 forest  = mix(lin(vec3(0.085, 0.13, 0.07)), lin(vec3(0.12, 0.15, 0.08)), n2)
                     * (0.8 + 0.4 * (fbmPx(xz, 14.0, px) + 0.5));
        vec3 rock    = mix(lin(vec3(0.38, 0.36, 0.34)), lin(vec3(0.52, 0.50, 0.47)), n1);
        // Strata: bands of lighter and darker rock that follow the height,
        // bent by the noise so they are not contour lines.
        rock *= 0.82 + 0.28 * smoothstep(0.3, 0.7, vnoise(vec2(h * 0.035 + n2 * 2.0, 0.5)));
        vec3 snow    = lin(vec3(0.93, 0.95, 0.98));

        vec3 albedo = meadow;
        // Above the tree line the forest thins into alpine grass.
        float alpineAmt = smoothstep(uTreeLine - 60.0, uTreeLine + 120.0, h + (n1 - 0.5) * 220.0);
        albedo = mix(albedo, alpine, alpineAmt);
        // Forest: where the ecology put the trees (the same stands the impostor
        // field and the near meshes stand in -- ecology.glsl), a closed canopy
        // once a cell in two or so carries a tree. Without it, the old rule:
        // on the slopes it can hold, below the tree line, in valley patches.
        float forestAmt;
        if (uEcoOn == 1) {
            vec3 e = ecoSample(xz, h, N.y);
            forestAmt = clamp(e.x * 1.3, 0.0, 1.0);
            forest = uCanopy * 0.75 * (0.75 + 0.5 * (fbmPx(xz, 14.0, px) + 0.5))
                   * mix(0.85, 1.1, n2);
        } else {
            float woods = smoothstep(0.28, 0.5, moist + (n2 - 0.5) * 0.35);
            float valleyPatch = mix(smoothstep(0.42, 0.62, n1 * 0.6 + n3 * 0.6), 1.0,
                                    smoothstep(40.0, 180.0, h));
            forestAmt = woods * valleyPatch * (1.0 - alpineAmt)
                      * (1.0 - smoothstep(30.0, 40.0, slope));
        }
        albedo = mix(albedo, forest, forestAmt);
        // Rock: where it is too steep for soil, and more of it the higher up.
        float rockAmt = smoothstep(30.0, 44.0, slope + (n2 - 0.5) * 16.0);
        rockAmt = max(rockAmt, alpineAmt * smoothstep(0.55, 0.8, n1) *
                               smoothstep(uTreeLine + 150.0, uTreeLine + 500.0, h));
        albedo = mix(albedo, rock, rockAmt);
        // Snow: above a line that wanders with the weather of each massif, and
        // only where it can lie -- the faces steeper than that stay dark rock,
        // which is what draws the ranges' structure from far away.
        float snowLine = uSnowLevel + (n3 - 0.5) * 320.0 + (n1 - 0.5) * 140.0;
        float snowAmt  = smoothstep(snowLine - 50.0, snowLine + 50.0, h)
                       * (1.0 - smoothstep(36.0, 50.0, slope + (n2 - 0.5) * 12.0));
        // High enough and it sticks nearly everywhere.
        snowAmt = max(snowAmt, smoothstep(snowLine + 350.0, snowLine + 700.0, h)
                               * (1.0 - smoothstep(55.0, 68.0, slope)));
        albedo = mix(albedo, snow, snowAmt);

        // Wet ground at the shore.
        albedo *= mix(1.0, 0.55, 1.0 - smoothstep(uWaterLevel, uWaterLevel + 1.5, h));

        float sh   = terrainShadow(P, N, L);
        float ndl  = max(dot(N, L), 0.0);
        // A forest canopy is rough on the metre scale: it scatters the light it
        // catches into its own shade, so it takes the sun flatter and darker.
        ndl = mix(ndl, sqrt(ndl) * 0.8, forestAmt);
        vec3  sky  = uAmbient * (0.55 + 0.45 * N.y);
        color = albedo * (uLightColor * ndl * sh + sky);
        // Snow glitters towards the sun a little.
        vec3  Hh   = normalize(L + V);
        color += uLightColor * snowAmt * sh * 0.08 * pow(max(dot(N, Hh), 0.0), 60.0);
    }

    color = applyFog(color, P, uCamPos, L);
    color = applyAir(color, P, uCamPos, L);
    FragColor = vec4(color, 1.0);
}
