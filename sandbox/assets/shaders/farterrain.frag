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
#include "cloudshadow.glsl"

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

// --- A forest seen from a few kilometres ------------------------------------
// Not a colour but crowns: a roof of rounded tops, each lit on its sun side and
// dark on the other, with shade in the gaps between them. At three kilometres
// a crown is still five or six pixels across, and a flat green there is what
// made the far woods read as moss. `crownH` is the canopy's height in crown
// heights (0 = the ground in a gap), on a jittered 7.5 m grid.
float crownH(vec2 p) {
    vec2  g = p / 7.5;
    vec2  i = floor(g), f = g - i;
    float h = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2  o = vec2(float(x), float(y));
            vec2  c = o + 0.1 + 0.8 * vec2(hash21(i + o), hash21(i + o + 17.3));
            float r = 0.45 + 0.30 * hash21(i + o + 41.7);
            float d = length(f - c) / r;
            h = max(h, (1.0 - d * d) * (0.7 + 0.6 * hash21(i + o + 9.1)));
        }
    return h;
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
// The sky's own gradient, the same function sky.frag draws (skyair.glsl): the
// colour a range dissolves into has to be the colour of the sky behind it, or
// every silhouette carries a seam.
#include "skyair.glsl"
vec3 skyAir(vec3 dir) { return skyGradient(dir, uLightDir); }

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
    air += uFogSunColor * pow(max(dot(rd, L), 0.0), 8.0) * 0.18;   // forward scatter
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
    vec3  N0 = N;                                           // the field's own normal
    float slope0 = degrees(acos(clamp(N.y, -1.0, 1.0)));   // ...and slope
    float bump  = fbmPx(xz, 55.0, px) * (4.0 + 26.0 * steep);
    // Gullies: what water and rockfall cut into a mountainside -- channels
    // running straight down it, a few dozen metres apart, ribs between them.
    // Noise stretched five to one along the fall line; the fall line turns
    // from place to place, so four fixed orientations are blended by how well
    // each matches it (turning ONE frame with the slope would swirl the
    // pattern, kilometres from the origin, into whorls).
    if (steep > 0.08) {
        vec2  dn   = normalize(N.xz + 1e-5);          // downhill
        float vis  = 1.0 - smoothstep(0.3, 0.9, px / 26.0);
        if (vis > 0.0) {
            float g = 0.0, wsum = 0.0;
            for (int k = 0; k < 4; ++k) {
                float a  = float(k) * 0.7853982;
                vec2  d  = vec2(cos(a), sin(a));
                float w  = pow(abs(dot(dn, d)), 6.0) + 1e-4;
                vec2  pp = vec2(dot(xz, vec2(-d.y, d.x)) / 26.0, dot(xz, d) / 130.0);
                float r1 = 1.0 - abs(2.0 * vnoise(pp + float(k) * 7.3) - 1.0);
                float r2 = 1.0 - abs(2.0 * vnoise(pp * vec2(2.3, 1.7) + 31.0) - 1.0);
                g    += w * (r1 * r1 + 0.45 * r2 * r2);
                wsum += w;
            }
            // Some faces are scored all over, some are smooth slabs.
            float face = 0.35 + 0.65 * smoothstep(0.3, 0.75, vnoise(xz / 900.0 + 3.7));
            bump += (g / wsum - 0.55) * 13.0 * face * smoothstep(0.08, 0.35, steep) * vis;
        }
    }
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
        vec3 meadow  = pow(meadowColour(xz, moist, px), vec3(2.2)) * uGrassTint;
        vec3 alpine  = mix(lin(vec3(0.33, 0.40, 0.19)), lin(vec3(0.45, 0.43, 0.24)), n2);
        vec3 forest  = mix(lin(vec3(0.085, 0.13, 0.07)), lin(vec3(0.12, 0.15, 0.08)), n2)
                     * (0.8 + 0.4 * (fbmPx(xz, 14.0, px) + 0.5));
        // Rock that differs from massif to massif -- warm limestone, cool
        // granite -- and within one face, lighter where it is fresh.
        vec3 rockCool = mix(lin(vec3(0.36, 0.36, 0.37)), lin(vec3(0.50, 0.50, 0.50)), n1);
        vec3 rockWarm = mix(lin(vec3(0.44, 0.39, 0.33)), lin(vec3(0.58, 0.53, 0.45)), n1);
        vec3 rock     = mix(rockCool, rockWarm, smoothstep(0.35, 0.65, n3));
        // Strata: faint bands that follow the height only loosely -- bent hard
        // by the noise and tilted across the range, so they read as bedding
        // in the rock and not as contour lines drawn on it.
        float bed = h * 0.028 + n2 * 5.0 + n1 * 3.0 + dot(xz, vec2(0.0011, -0.0007));
        rock *= 0.9 + 0.14 * smoothstep(0.35, 0.65, vnoise(vec2(bed, n3 * 3.0)));
        // Scree: the pale fans below the cliffs, on the slopes just too steep
        // for grass and not steep enough to stand as rock.
        vec3 scree = mix(lin(vec3(0.52, 0.50, 0.46)), lin(vec3(0.62, 0.59, 0.54)), n2);
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
            vec3 e = ecoSample(xz, h, N0.y);
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
        // The crowns, while they are big enough on screen to be crowns; past
        // that the roof's self-shadowing is the canopy darkening below.
        float crownVis = forestAmt * (1.0 - smoothstep(0.35, 1.2, px / 7.5));
        if (crownVis > 0.01) {
            const float kE = 0.8, kH = 5.0;             // metres: step, crown height
            float c0 = crownH(xz);
            float cx = crownH(xz + vec2(kE, 0.0));
            float cz = crownH(xz + vec2(0.0, kE));
            vec3  g  = vec3(cx - c0, 0.0, cz - c0) * (kH / kE);
            N = normalize(N - g * crownVis);
            float gap = 1.0 - smoothstep(0.0, 0.3, c0);
            albedo *= mix(1.0, 0.35, gap * crownVis);
        }
        // Rock: where it is too steep for soil, and more of it the higher up.
        float screeAmt = smoothstep(24.0, 32.0, slope + (n1 - 0.5) * 10.0)
                       * (1.0 - forestAmt) * smoothstep(uTreeLine - 200.0, uTreeLine + 100.0, h);
        albedo = mix(albedo, scree, screeAmt * 0.8);
        // Rock breaks through where it is too steep to hold soil: past about
        // 44 degrees under the forest (a mountain wood grows on slopes that
        // look impossible from below), 32 above the tree line. The field's
        // own slope decides, the gullies only fray the edge.
        float rockSlope = mix(44.0, 32.0, alpineAmt);
        float rockAmt = smoothstep(rockSlope, rockSlope + 12.0,
                                   mix(slope0, slope, 0.35) + (n2 - 0.5) * 16.0);
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

        float sh   = terrainShadow(P, N, L) * cloudLight(P);
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
