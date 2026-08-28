#version 330 core

#define MAX_CASCADES 4

in vec3  vWorldPos;
in vec3  vNormal;
in vec2  vUV;
in vec4  vPaint;      // terrain texture-paint weights for layers 0..3 (0 = auto)
in float vViewDepth;
out vec4 FragColor;

uniform vec3 uViewPos;
uniform vec3 uLightDir;    // direction *towards* the light, world space
uniform vec3 uLightColor;
uniform vec3 uAmbient;     // sky/fill light (drives day/night darkening)

// Atmospheric fog (aerial perspective + exponential height fog).
uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

// Output color management.
uniform float uExposure;
uniform int   uTonemap; // 1 = ACES tonemap + gamma (final), 0 = linear

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

// Shadows (cascaded).
uniform sampler2DArray uShadowMap;
uniform mat4  uLightSpace[MAX_CASCADES];
uniform float uCascadeSplits[MAX_CASCADES];
uniform int   uCascadeCount;

// Point lights (placed Light entities). Colours are HDR (colour * intensity).
#define MAX_POINT_LIGHTS 16
uniform int   uPointCount;
uniform vec3  uPointPos[MAX_POINT_LIGHTS];
uniform vec3  uPointColor[MAX_POINT_LIGHTS];
uniform float uPointRange[MAX_POINT_LIGHTS];

#define MAX_SPOT_LIGHTS 8
uniform int   uSpotCount;
uniform vec3  uSpotPos[MAX_SPOT_LIGHTS];
uniform vec3  uSpotDir[MAX_SPOT_LIGHTS];      // normalized cone axis
uniform vec3  uSpotColor[MAX_SPOT_LIGHTS];
uniform float uSpotRange[MAX_SPOT_LIGHTS];
uniform float uSpotCosInner[MAX_SPOT_LIGHTS]; // cos(inner half-angle): full bright
uniform float uSpotCosOuter[MAX_SPOT_LIGHTS]; // cos(outer half-angle): fades to zero

// Omnidirectional shadows for the first uShadowCount point lights.
uniform int   uShadowCount;
uniform samplerCube uShadowCube0;
uniform samplerCube uShadowCube1;
uniform samplerCube uShadowCube2;
uniform samplerCube uShadowCube3;
uniform float uShadowFar0;
uniform float uShadowFar1;
uniform float uShadowFar2;
uniform float uShadowFar3;
// Per-light normalized depth bias. Front-face culling in the cube pass already
// keeps acne away, so this is small: a larger bias detaches the shadow from the
// object ("peter panning", a visible gap at the contact point).
uniform float uShadowBias0;
uniform float uShadowBias1;
uniform float uShadowBias2;
uniform float uShadowBias3;

float pointShadow(int i, vec3 toFrag, float far, float bias) {
    float cur = length(toFrag) / far;
    float closest;
    if (i == 0)      closest = texture(uShadowCube0, toFrag).r;
    else if (i == 1) closest = texture(uShadowCube1, toFrag).r;
    else if (i == 2) closest = texture(uShadowCube2, toFrag).r;
    else             closest = texture(uShadowCube3, toFrag).r;
    return (cur - bias > closest) ? 1.0 : 0.0; // 1 = shadowed
}

// Environment reflection (dynamic scene cubemap probe).
uniform samplerCube uEnvProbe;
uniform float uEnvMaxLod;      // coarsest mip level (for rough reflections)
uniform float uReflectivity;   // 0 = matte (no reflection), 1 = mirror
uniform float uRoughness;      // 0 = sharp reflection, 1 = blurry

// Baked indirect light, looked up by WORLD POSITION rather than by a surface
// coordinate. Three volumes, one per colour channel, each texel holding that
// channel's four L1 spherical-harmonic coefficients as RGBA. The bake put the
// cosine convolution into the numbers, so reconstruction here is one dot
// product and a clamp -- there is no spherical-harmonic maths in this shader,
// deliberately, because the constants belong where they can be checked against
// an answer (see pathtrace::bakeProbes and pathcheck).
uniform sampler3D uLightGridR;
uniform sampler3D uLightGridG;
uniform sampler3D uLightGridB;
uniform int   uUseLightGrid;
uniform vec3  uLightGridLo;
uniform vec3  uLightGridHi;
uniform float uLightGridIntensity;

// What a Lambertian surface at `wp` facing `n` receives, per unit albedo --
// the same quantity uAmbient is, which is what lets it stand in for it.
vec3 bakedIrradiance(vec3 wp, vec3 n) {
    vec3 t = (wp - uLightGridLo) / max(uLightGridHi - uLightGridLo, vec3(1e-4));
    // Half a texel in from each face: a sample exactly on the boundary picks up
    // the clamp, and the outermost probes are the ones sitting in whatever the
    // grid was cut off by.
    vec3 dim = vec3(textureSize(uLightGridR, 0));
    t = clamp(t, 0.5 / dim, 1.0 - 0.5 / dim);
    vec4 r = texture(uLightGridR, t);
    vec4 g = texture(uLightGridG, t);
    vec4 b = texture(uLightGridB, t);
    vec4 basis = vec4(1.0, n.x, n.y, n.z);
    return max(vec3(dot(r, basis), dot(g, basis), dot(b, basis)), vec3(0.0))
           * uLightGridIntensity;
}

// Image-based lighting from an HDRI (diffuse irradiance + specular prefilter).
uniform int         uUseIBL;         // 1 = light ambient from the HDRI
uniform samplerCube uIrradiance;     // diffuse convolution
uniform samplerCube uPrefilter;      // specular, mipped by roughness
uniform float       uPrefilterMaxLod;
uniform float       uIBLIntensity;

// Karis' analytic environment BRDF (split-sum approximation, no LUT texture).
vec3 envBRDFApprox(vec3 F0, float rough, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + ab.y;
}

// Surface.
uniform sampler2D uTexture;   // used when uColorMode == 2
uniform int  uColorMode;      // 0 = uAlbedo, 1 = terrain palette, 2 = texture
// Multiplies the sampled base colour (uColorMode == 2). White leaves the map
// alone; every textured material must set it, or the previous draw's tint
// leaks in through the shared program.
uniform vec3 uTint;

// Road edge fade: alpha tapers to 0 over the outer `uRoadFade` metres of the
// ribbon so the road blends into the terrain instead of ending on a hard line.
// 0 disables it (the default for every non-road textured surface). vUV.x runs
// 0..uRoadUMax across the road width, so it maps linearly to metres across.
uniform float uRoadFade;      // fade band width in metres (0 = off)
uniform float uRoadWidth;     // road width in metres
uniform float uRoadUMax;      // vUV.x at the road's far edge (width / texTile)
uniform vec3 uAlbedo;
uniform float uAlpha;         // material opacity (1 = opaque); * texture alpha
uniform int   uGlass;         // 1 = Fresnel alpha (clear head-on, opaque rim)
uniform int   uAlphaCutout;   // 1 = discard fragments with texture alpha < uAlphaCutoff
uniform float uAlphaCutoff;   // cutout discard threshold (masked transparency)
uniform sampler2D uNormalMap; // tangent-space normal map (object materials)
uniform int   uHasNormalMap;  // 1 = perturb the normal with uNormalMap
uniform vec3  uEmission;         // emissive colour (sRGB); 0 = none
uniform float uEmissionStrength; // scales the emission (>1 for a strong glow)
uniform sampler2D uEmissionMap;  // optional emission mask/colour (Unity _Illum)
uniform int   uHasEmissionMap;   // 1 = modulate emission by uEmissionMap
// Scales vUV for the emission map only, so a glow map can tile differently from
// the base colour. (1,1) = same UVs as everything else, which is what object
// materials use. The road sets it so the map spans the carriageway exactly once
// across and repeats along the drive -- that is what keeps a painted centre
// line down the middle instead of tiling sideways with the asphalt.
uniform vec2  uEmissionUVScale;

// --- Procedural window grid -------------------------------------------------
// Lit windows on a facade, hashed straight out of the world position: no texture,
// no UVs and no extra geometry, which is what lets a generated tower stay at ~30
// objects while reading as an inhabited building instead of a smooth prism.
//
// WORLD SPACE, not vUV, and that is the whole reason this exists as a shader
// feature rather than a map: the generator's masses are scaled unit boxes whose
// UVs run 0..1 across a face of any size, so anything driven by them gives one
// building-sized window per facade instead of a facade full of building-scale
// windows. World position also lines the rows up across the setbacks of a stack.
uniform int   uWindowGrid;   // 1 = add window emission on the vertical faces
uniform vec2  uWindowCell;   // metres per window cell (across, up)
uniform float uWindowLit;    // fraction of the windows that are lit (0..1)
uniform float uWindowSeed;   // decorrelates districts that share one material
uniform vec3  uWindowColor;  // the warm end of the interior light (sRGB)
uniform float uWindowGlow;   // emission strength of a lit window

// Terrain palette (uColorMode == 1).
uniform vec3  uColorSand;
uniform vec3  uColorGrass;
uniform vec3  uColorRock;
uniform vec3  uColorSnow;
uniform float uSnowLevel;      // world height at which snow appears
uniform float uRockSlope;      // slope (normal.y) below which faces turn to rock
uniform float uSlopeSharpness; // blend width of the rock transition

// Data-driven terrain texture layers (uColorMode == 1). Each layer paints where
// a fragment's world height and surface slope both fall inside its band; layers
// with overlapping bands cross-fade. Fed from the terrain editor.
const int MAX_TERRAIN_LAYERS = 6;
uniform sampler2D uLayerTex[MAX_TERRAIN_LAYERS];
uniform int       uLayerCount;
uniform vec4      uLayerBand[MAX_TERRAIN_LAYERS];  // (hStart, hEnd, slopeStart, slopeEnd deg)
uniform float     uLayerScale[MAX_TERRAIN_LAYERS]; // triplanar tiling per layer
uniform sampler2D uLayerNorm[MAX_TERRAIN_LAYERS];  // optional per-layer normal map
uniform int       uLayerHasNorm[MAX_TERRAIN_LAYERS]; // 1 = layer i has a normal map
uniform float     uTexScale;   // world units -> texture tiling (fallback)
uniform float     uNormalStrength; // 0 = geometry normal, 1 = full normal-map relief
uniform float     uWaterLevel;     // surfaces below this are wet (darker)
uniform float     uWetness;        // rain wetness 0..1 (sky-facing gets dark+glossy)
uniform float     uRainRings;      // drop-impact rings 0..1 (0 = off; the road sets it)
uniform float     uTime;           // seconds, for the rings' clock
// Wetness variation: a greyscale map that says where the water stands. Only the
// road sets these; every other draw gets uHasWetMap = 0 from the renderer's
// baseline, so a shared program cannot leak the road's puddles onto a wall.
uniform sampler2D uWetMap;
uniform int       uHasWetMap;      // 1 = modulate the wetness by uWetMap
uniform vec2      uWetMapScale;    // ribbon UV -> map UV (tiles it in metres)
uniform float     uWetVar;         // 0 = even sheen .. 1 = fully map-driven
uniform float     uWetReflect;     // how much wetness mirrors the env probe (0 = off)
uniform float     uWetShore;       // puddle edge width in map units (small = hard rim)
// x = ribbon u at the left kerb, y = 1/(u across the carriageway). y = 0 switches
// the road-shaped pooling (gutter + wheel ruts) off, which is what a bridge deck
// gets: its UVs are not the ribbon's, so the gutters would land anywhere.
uniform vec2      uWetLat;

// Procedural micro-detail (uColorMode == 1).
uniform float uDetailScale;    // frequency of the close-up detail
uniform float uDetailStrength; // how strongly it perturbs the normal
uniform float uTerrainSpec;    // terrain sun-specular strength (0 = matte)

int selectCascade() {
    for (int i = 0; i < uCascadeCount; ++i) {
        if (vViewDepth < uCascadeSplits[i]) return i;
    }
    return uCascadeCount - 1;
}

// Interleaved gradient noise -- the per-pixel rotation for the PCF kernel below.
float ignRot(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// World size of one shadow texel in `layer` (the light matrix is orthographic, so
// its column length is the world -> light-space scale).
float shadowTexelWorld(int layer) {
    float scale = length(uLightSpace[layer][0].xyz);
    return 2.0 / max(scale * float(textureSize(uShadowMap, 0).x), 1.0e-4);
}

// One cascade's PCF lookup at an already normal-offset world position.
float shadowPcf(int layer, vec3 wp, float ndl, float rot) {
    vec4 lsPos = uLightSpace[layer] * vec4(wp, 1.0);
    vec3 proj  = lsPos.xyz / lsPos.w * 0.5 + 0.5;
    if (proj.z > 1.0) return 0.0;

    // Kept small: the normal offset below does the acne work geometrically, and
    // glPolygonOffset in the depth pass covers the rest. A large depth bias only
    // detaches the shadow from its caster (peter-panning).
    float bias = max(0.0010 * (1.0 - ndl), 0.00025) * (1.0 + float(layer) * 0.35);

    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0).xy);
    // Rotate the 5x5 tap grid per pixel. A fixed grid quantises the penumbra into
    // the same handful of levels everywhere, which reads as stepped, blocky
    // shadow edges; rotating it turns those steps into a fine dither that the eye
    // (and FXAA) resolve as a smooth gradient.
    float s = sin(rot), c = cos(rot);
    mat2  R = mat2(c, -s, s, c);
    const float spread = 1.35;

    float shadow = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 o = R * (vec2(x, y) * spread) * texel;
            float closest = texture(uShadowMap, vec3(proj.xy + o, float(layer))).r;
            shadow += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return shadow / 25.0;
}

float computeShadow(int layer, vec3 N, vec3 L) {
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float rot = 6.2831853 * ignRot(gl_FragCoord.xy);

    // Normal offset: move the lookup off the surface by about one shadow texel,
    // more as the light grazes. This is what actually removes acne -- and unlike a
    // depth bias it doesn't slide the shadow away from the object's contact point.
    vec3 wp = vWorldPos + N * shadowTexelWorld(layer) * (1.0 + 2.0 * (1.0 - ndl));
    float shadow = shadowPcf(layer, wp, ndl, rot);

    // Cascade blend: cross-fade into the next cascade over the last stretch of
    // this one. Without it the filter width (and bias) jump at the split, and the
    // jump draws a visible line straight across the ground.
    if (layer + 1 < uCascadeCount) {
        float split = uCascadeSplits[layer];
        float prev  = (layer == 0) ? 0.0 : uCascadeSplits[layer - 1];
        float t = smoothstep(mix(prev, split, 0.85), split, vViewDepth);
        if (t > 0.001) {
            vec3 wp2 = vWorldPos +
                       N * shadowTexelWorld(layer + 1) * (1.0 + 2.0 * (1.0 - ndl));
            shadow = mix(shadow, shadowPcf(layer + 1, wp2, ndl, rot), t);
        }
    }
    return shadow;
}

// --- Cheap procedural value-noise fBm for surface micro-detail -------------
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1, 0));
    float c = hash21(i + vec2(0, 1));
    float d = hash21(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float detailFbm(vec2 p) {
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * vnoise(p);
        p   *= 2.0;
        amp *= 0.5;
    }
    return sum;
}

// --- Window grid ------------------------------------------------------------
// The emissive contribution of the procedural windows at this fragment, in
// linear light and already scaled by uWindowGlow. Zero on horizontal faces.
vec3 windowEmission(vec3 wp, vec3 n) {
    // Roofs and soffits get none. A horizontal face has no windows, and a grid
    // running over one is the giveaway the moment a track climbs above the
    // district -- which in this engine it does.
    float wall = 1.0 - smoothstep(0.55, 0.75, abs(n.y));
    if (wall <= 0.0) return vec3(0.0);

    // Run the grid along whichever horizontal axis the wall does NOT face, so
    // the windows read across it on all four sides of a box.
    float across = (abs(n.x) > abs(n.z)) ? wp.z : wp.x;
    vec2  cell   = vec2(across, wp.y) / max(uWindowCell, vec2(0.5));
    vec2  id     = floor(cell);
    vec2  f      = fract(cell);
    // Keep the hash's input small. hash21 is a fract() of a product, so a district
    // laid out a few thousand metres from the origin hands it numbers big enough
    // that float precision collapses the pattern into visible bands of identical
    // windows. Wrapping the cell id repeats the layout every ~1.7 km instead,
    // which is a repeat nobody is going to catch at 500 km/h.
    id = mod(id, 512.0);
    vec2  w      = fwidth(cell) + 1e-5;

    // The pane sits inside the cell; the margin left over is the mullion and the
    // floor slab, and it is that dark border which makes a window a window.
    // Edges softened by their own screen-space width: these are hard rectangles
    // on a facade crossed at 500 km/h, and unfiltered they crawl with sparkle.
    float pane = smoothstep(0.16 - w.x, 0.16 + w.x, f.x)
               * (1.0 - smoothstep(0.84 - w.x, 0.84 + w.x, f.x))
               * smoothstep(0.22 - w.y, 0.22 + w.y, f.y)
               * (1.0 - smoothstep(0.82 - w.y, 0.82 + w.y, f.y));

    vec3 warm = pow(uWindowColor, vec3(2.2));
    vec3 cold = vec3(0.55, 0.68, 0.95);   // fluorescent office white

    // Whole floors go dark together now and then (plant rooms, empty storeys):
    // a per-ROW draw on top of the per-window one, because fully independent
    // windows read as television static rather than as a building at dusk.
    float rowLit = step(0.12, hash21(vec2(id.y, uWindowSeed * 7.13)));
    float lit    = step(hash21(id + uWindowSeed), uWindowLit) * rowLit;

    // Per-window variation in brightness and colour temperature, keyed off the
    // same cell through different bands of the hash.
    float v    = hash21(id.yx + uWindowSeed * 3.7);
    float cool = hash21(id * 1.37 + uWindowSeed * 11.9);
    vec3  near = mix(warm, cold, cool * 0.55) * (0.45 + 0.85 * v) * lit * pane;

    // Beyond the distance where a cell covers a pixel, stop resolving individual
    // windows and fade to what the grid integrates to (mean brightness * lit
    // fraction * pane area * the rows that stayed lit). A distant facade should
    // be an even glow, and asking for per-window detail there buys only aliasing.
    vec3  avg = mix(warm, cold, 0.28) * 0.875 * uWindowLit * 0.408 * 0.88;
    float far = clamp(max(w.x, w.y) * 1.6 - 0.35, 0.0, 1.0);
    return mix(near, avg, far) * uWindowGlow * wall;
}

// Triplanar sampling: project the texture along the three world axes and blend
// by the (squared) normal, so steep terrain doesn't stretch a flat UV.
//
// The derivatives are passed IN (dpdx/dpdy of the world position) and the samples
// use textureGrad rather than plain texture(). That is not a micro-optimisation:
// these calls sit inside the layer loop's `if (w > 0.0)`, and a texture() with
// implicit LOD inside non-uniform control flow has *undefined* derivatives --
// neighbouring pixels of a quad take different branches, so the finite difference
// the hardware forms is garbage and it picks a wrong mip. Since the three
// projections mix per pixel, the wrongness lands diagonally and swims as the
// camera turns: the diagonal, view-direction-dependent moire on the terrain.
// Wrap a scaled world-space UV into [0,1). With GL_REPEAT this samples exactly
// the same texels -- but far from the origin the un-wrapped coordinate is a big
// float with barely any sub-texel bits left, which quantises the filter into
// bands. Safe only because the sampling below passes its derivatives explicitly:
// a fract() seam would otherwise wreck the implicit LOD along one pixel row.
vec2 wrapUv(vec2 uv) { return fract(uv); }

vec3 triplanar(sampler2D tex, vec3 wp, vec3 n, float scale, vec3 dpdx, vec3 dpdy) {
    vec3 bw = abs(n);
    bw = pow(bw, vec3(4.0));
    bw /= (bw.x + bw.y + bw.z);
    vec3 cx = textureGrad(tex, wrapUv(wp.zy * scale), dpdx.zy * scale, dpdy.zy * scale).rgb;
    vec3 cy = textureGrad(tex, wrapUv(wp.xz * scale), dpdx.xz * scale, dpdy.xz * scale).rgb;
    vec3 cz = textureGrad(tex, wrapUv(wp.xy * scale), dpdx.xy * scale, dpdy.xy * scale).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}

// Triplanar normal mapping (Whiteout blend): reorient each plane's tangent-space
// normal onto the geometry normal, then blend by the (squared) normal. Same
// explicit-derivative treatment as the colour path above.
vec3 triplanarNormal(sampler2D nmap, vec3 wp, vec3 n, float scale,
                     vec3 dpdx, vec3 dpdy) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= (bw.x + bw.y + bw.z);
    vec3 tx = textureGrad(nmap, wrapUv(wp.zy * scale), dpdx.zy * scale, dpdy.zy * scale).xyz * 2.0 - 1.0;
    vec3 ty = textureGrad(nmap, wrapUv(wp.xz * scale), dpdx.xz * scale, dpdy.xz * scale).xyz * 2.0 - 1.0;
    vec3 tz = textureGrad(nmap, wrapUv(wp.xy * scale), dpdx.xy * scale, dpdy.xy * scale).xyz * 2.0 - 1.0;
    tx = vec3(tx.xy + n.zy, abs(tx.z) * n.x);
    ty = vec3(ty.xy + n.xz, abs(ty.z) * n.y);
    tz = vec3(tz.xy + n.xy, abs(tz.z) * n.z);
    return normalize(tx.zyx * bw.x + ty.xzy * bw.y + tz.xyz * bw.z);
}

// One layer's coverage: 1 inside its [start,end] band, smoothly 0 outside. A
// fixed feather softens both edges so adjacent bands cross-fade.
float band(float x, float start, float end, float feather) {
    return smoothstep(start - feather, start + feather, x) *
           (1.0 - smoothstep(end - feather, end + feather, x));
}

// Triplanar-sample a layer by index using CONSTANT sampler indices only --
// GLSL 3.30 forbids indexing a sampler array with a non-constant expression.
vec3 layerTriplanar(int i, vec3 wp, vec3 n, float scale, vec3 dpdx, vec3 dpdy) {
    if (i == 0) return triplanar(uLayerTex[0], wp, n, scale, dpdx, dpdy);
    if (i == 1) return triplanar(uLayerTex[1], wp, n, scale, dpdx, dpdy);
    if (i == 2) return triplanar(uLayerTex[2], wp, n, scale, dpdx, dpdy);
    if (i == 3) return triplanar(uLayerTex[3], wp, n, scale, dpdx, dpdy);
    if (i == 4) return triplanar(uLayerTex[4], wp, n, scale, dpdx, dpdy);
    return triplanar(uLayerTex[5], wp, n, scale, dpdx, dpdy);
}

// Same constant-index dispatch for the per-layer normal maps.
vec3 layerTriplanarNormal(int i, vec3 wp, vec3 n, float scale, vec3 dpdx, vec3 dpdy) {
    if (i == 0) return triplanarNormal(uLayerNorm[0], wp, n, scale, dpdx, dpdy);
    if (i == 1) return triplanarNormal(uLayerNorm[1], wp, n, scale, dpdx, dpdy);
    if (i == 2) return triplanarNormal(uLayerNorm[2], wp, n, scale, dpdx, dpdy);
    if (i == 3) return triplanarNormal(uLayerNorm[3], wp, n, scale, dpdx, dpdy);
    if (i == 4) return triplanarNormal(uLayerNorm[4], wp, n, scale, dpdx, dpdy);
    return triplanarNormal(uLayerNorm[5], wp, n, scale, dpdx, dpdy);
}

// Height- and slope-driven terrain surface: blends the configured texture layers
// by coverage, producing both the albedo and the perturbed normal in one pass.
// Layer coverage (height band x slope band) weights both, so a layer's normal
// map only shows where its texture shows. uNormalStrength dials the relief in.
void terrainSurface(vec3 wp, vec3 n, float detail, out vec3 albedo, out vec3 normalOut) {
    normalOut = n;
    if (uLayerCount == 0) { albedo = uAlbedo; return; } // no layers -> flat base

    // Screen-space derivatives of the world position, taken HERE -- in uniform
    // control flow -- and handed to every triplanar sample below. See triplanar().
    vec3 dpdx = dFdx(wp);
    vec3 dpdy = dFdy(wp);

    float slopeDeg = degrees(acos(clamp(n.y, -1.0, 1.0))); // 0 flat .. 90 vertical
    float h        = wp.y + (detail - 0.5) * 3.0;          // jitter the height edges

    // Manual texture paint (layers 0..3): `paintCover` is how much of this fragment
    // was hand-painted. Where painted, the automatic height/slope weights are faded
    // out and the painted layer weights take over; unpainted (paint == 0) leaves the
    // automatic blend untouched, so this is a pure additive override.
    vec4  paint      = clamp(vPaint, 0.0, 1.0);
    float paintCover = clamp(paint.x + paint.y + paint.z + paint.w, 0.0, 1.0);

    vec3  acc  = vec3(0.0);
    vec3  nacc = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < MAX_TERRAIN_LAYERS; ++i) {
        if (i >= uLayerCount) break;
        vec4  b = uLayerBand[i];
        float autoW = band(h, b.x, b.y, 1.5) * band(slopeDeg, b.z, b.w, 6.0);
        float pw    = (i < 4) ? paint[i] : 0.0;
        float w     = autoW * (1.0 - paintCover) + pw;
        if (w > 0.0) {
            acc += layerTriplanar(i, wp, n, uLayerScale[i], dpdx, dpdy) * w;
            vec3 ln = (uLayerHasNorm[i] == 1)
                    ? layerTriplanarNormal(i, wp, n, uLayerScale[i], dpdx, dpdy) : n;
            nacc += ln * w;
            wsum += w;
        }
    }
    if (wsum < 1e-4) { albedo = uAlbedo; return; } // gap between bands -> flat base
    albedo = acc / wsum;
    vec3 mapped = normalize(nacc / wsum);
    normalOut = normalize(mix(n, mapped, clamp(uNormalStrength, 0.0, 1.0)));
}

// Apply exponential height fog with sun-tinted in-scatter to a shaded colour.
vec3 applyFog(vec3 color, vec3 worldPos, vec3 eye, vec3 lightDir) {
    vec3  toFrag = worldPos - eye;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);

    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(eye.y - uFogHeight) * b);
    float od;
    if (abs(rd.y) > 1e-4) od = c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y);
    else                  od = c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));

    float sunAmt = pow(max(dot(rd, normalize(lightDir)), 0.0), 4.0);
    vec3  fogCol = mix(uFogColor, uFogSunColor, sunAmt);
    return mix(color, fogCol, clamp(fog, 0.0, 1.0));
}

// Perturb the geometry normal `N` by a tangent-space normal map, building the
// TBN basis from screen-space derivatives (no per-vertex tangents needed).
// --- Rain rings ---------------------------------------------------------------
// Drops landing on wet ground, faked outright: no particles, no geometry, no extra
// draw call. World space is cut into cells, each cell drips once per cycle staggered
// by its own hash, and a ring spreads from a random spot inside it.
//
// The wave's radial slope goes straight into the normal instead of building a height
// field and differencing it -- the derivative of a sine is another sine, so one cheap
// term per drop replaces three samples. Nothing is stored between frames: a drop's
// whole life is a function of the clock and its cell -- hash21 above is the whole
// state.
// `amount` is the drop rate times the user's strength dial, so it runs past 1.
vec3 rainRings(vec3 N, vec2 wp, float amount, float time) {
    if (amount <= 0.002) return N;      // dry: not a single hash
    const float kCell  = 0.5;           // metres between drops
    const float kFreq  = 34.0;          // ring wavelength
    const float kRate  = 1.9;           // drops per second per cell
    // Tilt at full strength. ~0.4 of lateral push on a unit normal is about 11
    // degrees -- enough to catch the sun. (The first cut used 0.05, i.e. 1.5
    // degrees, which is invisible on anything but a mirror.)
    const float kTilt  = 0.4;
    vec2 cell = floor(wp / kCell);
    vec2 tilt = vec2(0.0);
    // 3x3 neighbourhood: a ring reaches half a cell, so a drop can never expand in
    // from further out than its neighbours -- no popping at the cell borders.
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2  c   = cell + vec2(x, y);
            float age = fract(time * kRate + hash21(c)); // its own schedule
            vec2  at  = (c + vec2(hash21(c + 7.1), hash21(c + 3.7))) * kCell;
            vec2  d   = wp - at;
            float r   = length(d);
            float f   = age * kCell * 0.5;               // the expanding front
            if (r > f) continue;                          // ahead of it: still flat
            // A decaying ripple behind the front, dying with the drop. The decay is
            // gentle enough that the ring still reads at its outer edge -- at -7 it
            // had lost 83% of its amplitude by the time it got there, so only the
            // very centre of each drop was doing any work.
            float w = cos((r - f) * kFreq) * exp(-4.0 * r) * (1.0 - age);
            tilt += (d / max(r, 1e-4)) * w;
        }
    }
    N.xz += tilt * amount * kTilt; // gated to up-facing ground by the caller
    return normalize(N);
}

vec3 applyNormalMap(vec3 N, vec3 worldPos, vec2 uv, sampler2D nmap) {
    vec3 nt = texture(nmap, uv).xyz * 2.0 - 1.0; // tangent-space normal
    vec3 dp1 = dFdx(worldPos), dp2 = dFdy(worldPos);
    vec2 du1 = dFdx(uv),       du2 = dFdy(uv);
    vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    vec3 T = dp2p * du1.x + dp1p * du2.x;
    vec3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = inversesqrt(max(dot(T, T), dot(B, B)));
    return normalize(mat3(T * inv, B * inv, N) * nt);
}

void main() {
    vec3 N = normalize(vNormal); // smooth geometry normal (drives material masks)

    // Procedural micro-detail. It has no mip chain to fall back on, so once a
    // pixel covers more than about one noise period it aliases -- a fixed
    // world-space frequency beating against the pixel grid, which is the other
    // half of the terrain's shimmer. Fade it to its mean (0.5, the neutral value
    // for every consumer) exactly where the footprint outruns it.
    float detail = 0.5;
    if (uColorMode == 1) {
        vec2  dp   = fwidth(vWorldPos.xz * uDetailScale); // periods per pixel
        float fade = 1.0 - smoothstep(0.35, 1.0, max(dp.x, dp.y));
        detail = mix(0.5, detailFbm(vWorldPos.xz * uDetailScale), fade);
    }

    vec3 albedo;
    vec3 terrainNrm = N; // terrain's perturbed normal (filled in layer mode)
    float texA = 1.0; // texture alpha, folded into the output alpha
    if (uColorMode == 1) {
        terrainSurface(vWorldPos, N, detail, albedo, terrainNrm);
    } else if (uColorMode == 2) {
        vec4 t = texture(uTexture, vUV);
        albedo = t.rgb * uTint; texA = t.a;
    } else {
        albedo = uAlbedo;
    }
    // Masked transparency ("transparency map", Cutout mode): drop fully/mostly
    // transparent texels before any lighting so they never write colour or depth.
    if (uAlphaCutout == 1 && texA < uAlphaCutoff) discard;
    albedo = pow(albedo, vec3(2.2)); // sRGB -> linear for correct lighting

    // Submerged (and just-above-waterline) surfaces are wet -> darker, with a
    // narrow damp band reaching slightly above the surface.
    float wet = 1.0 - smoothstep(uWaterLevel - 0.8, uWaterLevel + 0.4, vWorldPos.y);
    albedo *= mix(1.0, 0.42, wet);

    // Surface normal: terrain uses its own path; object materials optionally
    // perturb the geometry normal with a tangent-space normal map.
    if (uColorMode == 1) {
        N = terrainNrm;
    } else if (uHasNormalMap == 1) {
        N = applyNormalMap(N, vWorldPos, vUV, uNormalMap);
    }

    // Rain wetness, in two parts, because a wet road is two materials at once.
    // `rainWet` is the film: the sheet of water that darkens everything sky-facing
    // and gives it a broad sheen. `wetPuddle` is standing water, and that is the
    // half that mirrors -- water lying in a hollow is FLAT, so it keeps a
    // reflection together, while the film sits on tarmac grain that shatters one.
    // Both are gated by the up-facing normal so walls and undersides stay dry.
    float upFace  = clamp(N.y * 1.3, 0.0, 1.0);
    float rainWet = uWetness * upFace;
    float wetMask = 1.0;   // where water STANDS, independent of how hard it rains
    // Only a surface carrying a wetness map has water standing on it. Everything
    // else -- terrain, props, a road with no map -- keeps the even sheen it had
    // before, which is why the puddle-only terms below are all gated on this.
    bool  wetStands = (uHasWetMap == 1 && uWetVar > 0.0);
    // A road is never evenly wet: water pools where the camber lets it and runs
    // off everywhere else, so a uniformly mirrored carriageway reads as plastic
    // however good the reflection is.
    if (wetStands) {
        // The map is read as a HEIGHT FIELD, not as a wetness multiplier: black is
        // a hollow, white a crown. Water then fills it up to a level the rain
        // raises, which is what gives a puddle a shoreline instead of a soft grey
        // smear -- and what makes puddles grow while it keeps raining.
        //
        // Two taps at incommensurable scales. The fine one is the shape the eye
        // reads close up; the coarse one (a quarter of the frequency) is the
        // large-scale "this stretch drains badly" structure, and it is the only
        // part that survives mipping far down the road -- which is exactly where
        // the reflection is most visible and where a single tap averages out to
        // an even grey, i.e. back to the look this whole map exists to break.
        float h = texture(uWetMap, vUV * uWetMapScale).r * 0.65
                + texture(uWetMap, vUV * uWetMapScale * 0.23 + vec2(0.37, 0.11)).r * 0.35;
        // Where the road itself holds water. Noise alone always reads as a texture
        // laid over the ribbon; the gutter along both kerbs and the two wheel ruts
        // are structure that belongs to the road, and they are what stops the
        // puddles looking printed on.
        if (uWetLat.y > 0.0) {
            float lat    = clamp((vUV.x - uWetLat.x) * uWetLat.y, 0.0, 1.0);
            float gutter = smoothstep(0.14, 0.0, min(lat, 1.0 - lat));
            float ruts   = exp(-pow((abs(lat - 0.5) - 0.22) * 14.0, 2.0));
            h -= 0.35 * gutter + 0.20 * ruts;
        }
        float level = mix(-0.05, 1.05, uWetness);   // the rain fills the hollows
        float depth = clamp((level - h) / max(uWetShore, 0.01), 0.0, 1.0);
        wetMask = mix(1.0, depth, clamp(uWetVar, 0.0, 1.0));
        // The film thins where the water has drained away, but never as far as the
        // puddles do: damp tarmac beside a puddle is still damp.
        rainWet *= mix(1.0, mix(0.45, 1.0, depth), clamp(uWetVar, 0.0, 1.0));
    }
    float wetPuddle = rainWet * wetMask;
    albedo *= mix(1.0, 0.6, rainWet);
    if (wetStands) albedo *= mix(1.0, 0.82, wetPuddle); // water on top, darker still

    // Standing water is a surface in its own right: flat, whatever the tarmac
    // under it does. Pulling the shading normal back towards the geometric one
    // inside a puddle is what actually sells the variation -- the reflection stays
    // coherent in the water and breaks up on the grain beside it. Without this a
    // puddle is only "the same reflection, brighter", which is the plastic look.
    // Never all the way: even standing water has a little relief.
    if (wetStands && wetPuddle > 0.0)
        N = normalize(mix(N, normalize(vNormal), wetPuddle * 0.9));

    // Drops striking the surface -- only where there is water to ring. After the
    // flattening so the rings sit on the water rather than on the tarmac, and
    // before the lighting so they catch the sheen: that glinting is most of what
    // sells them, and the geometry never moves. uRainRings already carries the
    // rain intensity and the wetness, so the gate here is the standing-water mask
    // and the up-facing normal, NOT the wetness a second time.
    if (uRainRings > 0.002)
        N = rainRings(N, vWorldPos.xz, uRainRings * upFace * wetMask, uTime);

    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 H = normalize(L + V);

    float diff      = max(dot(N, L), 0.0);
    // Terrain: sun-glint strength is art-directed via uTerrainSpec (0 = fully
    // matte). Textured surfaces (roads): rough/matte, faint broad sheen.
    float specPower = (uColorMode == 1) ? uTerrainSpec : 0.03;
    float specExp   = (uColorMode == 1) ? 48.0 : 14.0;
    // Wet surfaces gain a stronger, tighter specular highlight (the sheen).
    specPower = mix(specPower, max(specPower, 0.9), rainWet);
    specExp   = mix(specExp, 160.0, rainWet);
    // ...and standing water tighter still: a film scatters the sun into a broad
    // sheen, a puddle throws back a small hard disc of it.
    if (wetStands) specExp = mix(specExp, 600.0, wetPuddle);
    // Geometric specular anti-aliasing. Where the normal swings a lot *within one
    // pixel* -- distant normal-mapped detail, thin or dense geometry, a grazing
    // road -- a tight highlight is smaller than the pixel and turns into crawling
    // sparkle/moire. Widen the lobe by the pixel's normal variance instead, which
    // is what integrating over the pixel would have done anyway. (Kaplanyan's
    // roughness clamp, expressed for a Phong exponent.)
    {
        float variance = 0.25 * (dot(dFdx(N), dFdx(N)) + dot(dFdy(N), dFdy(N)));
        float alpha    = sqrt(2.0 / (specExp + 2.0));           // exponent -> width
        alpha          = sqrt(min(alpha * alpha + variance, 1.0));
        specExp        = max(2.0 / (alpha * alpha) - 2.0, 1.0); // ...and back
    }
    float spec      = pow(max(dot(N, H), 0.0), specExp) * specPower;

    int   layer   = selectCascade();
    float shadow  = computeShadow(layer, N, L);

    // Ambient: image-based lighting from the HDRI when enabled, else the flat
    // directional ambient. IBL gives diffuse irradiance + a soft env specular.
    vec3 ambient;
    if (uUseLightGrid == 1) {
        // The baked grid wins over both of the others where it exists, because
        // it is the only one of the three that knows WHERE the surface is. A
        // flat ambient lights the inside of a tunnel exactly as brightly as an
        // open field; an HDRI convolution does the same, only in colour.
        ambient = albedo * bakedIrradiance(vWorldPos, N);
    } else if (uUseIBL == 1) {
        float NoV = max(dot(N, V), 0.0);
        vec3  R   = reflect(-V, N);
        vec3  F0  = vec3(0.04);
        float iblRough = (uColorMode == 1) ? 0.9 : 0.55; // terrain matte, else semi
        vec3 diffuseIBL = texture(uIrradiance, N).rgb * albedo;
        vec3 preSpec    = textureLod(uPrefilter, R, iblRough * uPrefilterMaxLod).rgb;
        vec3 specIBL    = preSpec * envBRDFApprox(F0, iblRough, NoV);
        ambient = (diffuseIBL + specIBL) * uIBLIntensity;
    } else {
        ambient = albedo * uAmbient;
    }

    vec3 color = ambient
               + (1.0 - shadow) * uLightColor * (albedo * diff + spec);

    // Wet grazing sheen: brighten at glancing angles with the ambient/sky tint --
    // a cheap, probe-free "mirror" that sells the wet-road look.
    if (rainWet > 0.0) {
        float NoVw = max(dot(N, V), 0.0);
        float fres = pow(1.0 - NoVw, 4.0);
        color += ambient * (fres * rainWet * 2.0);
    }

    // Point lights: diffuse + a little specular, with smooth range falloff.
    for (int i = 0; i < uPointCount; ++i) {
        vec3  d   = uPointPos[i] - vWorldPos;
        float dst = length(d);
        vec3  Lp  = d / max(dst, 1e-4);
        float att = clamp(1.0 - dst / max(uPointRange[i], 1e-3), 0.0, 1.0);
        att *= att; // quadratic-ish falloff
        float dp  = max(dot(N, Lp), 0.0);
        float sp  = pow(max(dot(N, normalize(Lp + V)), 0.0), 32.0) * 0.2;
        float sh  = 0.0;
        if (i < uShadowCount) {
            float far = (i == 0) ? uShadowFar0 : (i == 1) ? uShadowFar1
                      : (i == 2) ? uShadowFar2 : uShadowFar3;
            float bias = (i == 0) ? uShadowBias0 : (i == 1) ? uShadowBias1
                       : (i == 2) ? uShadowBias2 : uShadowBias3;
            sh = pointShadow(i, -d, far, bias); // -d = light -> fragment
        }
        color += uPointColor[i] * (albedo * dp + sp) * att * (1.0 - sh);
    }

    // Spot lights: point-light falloff gated by a cone around the spot axis. The
    // cone ramps from full bright inside uSpotCosInner to zero past uSpotCosOuter.
    for (int i = 0; i < uSpotCount; ++i) {
        vec3  d   = uSpotPos[i] - vWorldPos;
        float dst = length(d);
        vec3  Lp  = d / max(dst, 1e-4);            // fragment -> light
        float att = clamp(1.0 - dst / max(uSpotRange[i], 1e-3), 0.0, 1.0);
        att *= att;
        // Angle between the cone axis and the light->fragment direction.
        float cosA = dot(-Lp, normalize(uSpotDir[i]));
        float cone = clamp((cosA - uSpotCosOuter[i]) /
                           max(uSpotCosInner[i] - uSpotCosOuter[i], 1e-3), 0.0, 1.0);
        cone *= cone; // smooth the cone edge
        float dp  = max(dot(N, Lp), 0.0);
        float sp  = pow(max(dot(N, normalize(Lp + V)), 0.0), 48.0) * 0.25;
        color += uSpotColor[i] * (albedo * dp + sp) * att * cone;
    }

    // A wet surface is a mirror in its own right: water fills the pores of the
    // tarmac and what is left is a smooth film, which is why a wet road shows the
    // city back and a dry one only shows the sun. So wetness feeds the same probe
    // path an authored reflective material uses -- but only upward, so a mirror
    // stays a mirror in the rain instead of being dulled by it.
    //
    // uWetReflect gates it, and every draw gets 0 from the renderer's baseline:
    // ONLY the road turns this on. Driving it from uWetness alone made every
    // surface in the scene mirror as soon as it rained -- the terrain, the props,
    // the car -- which is not what a wet road looks like, and which fed the probe
    // its own output frame after frame.
    //
    // Capped well short of 1: water reflects a few percent head-on and only turns
    // properly mirror-like at grazing angles, which the Fresnel term below already
    // does. Pushed higher, a wet road reads as polished chrome.
    float refl  = uReflectivity;
    float rough = uRoughness;
    //
    // Which half of the wetness mirrors matters more than how much of it there is:
    // the film scatters (blurred and weak), standing water does not (sharp and
    // strong). Driving both from one scalar was why a puddle used to read as
    // nothing more than a brighter patch of the same reflection.
    float wetRefl = clamp(uWetReflect, 0.0, 1.0)
                  * mix(rainWet * 0.25, wetPuddle, wetMask);
    if (wetRefl > refl) {
        refl  = wetRefl;
        rough = mix(0.55, 0.03, wetPuddle); // film = smeared, puddle = mirror
    }

    // Environment reflection: sample the dynamic scene probe along the reflection
    // vector and blend in by a Fresnel term. `refl` raises the base reflectance
    // F0 (0 -> dielectric 4%, 1 -> full mirror); `rough` selects a blurrier mip.
    // Reflection happens before fog so distant mirrors haze too.
    if (refl > 0.0) {
        vec3  Rv   = reflect(-V, N);
        vec3  env  = textureLod(uEnvProbe, Rv, rough * uEnvMaxLod).rgb;
        // The probe is an HDR cube that may never have been rendered this run,
        // and a reflective surface drawn INTO it samples the previous capture --
        // so one NaN or Inf in it feeds itself and spreads. Bloom then divides by
        // the luminance and hands the whole pyramid NaN, which is what turns the
        // screen into blocks of black. Sanitise on read: it costs nothing and it
        // is the only place this can be contained.
        if (any(isnan(env)) || any(isinf(env))) env = vec3(0.0);
        env = min(env, vec3(64.0));   // a reflection is never brighter than this
        float F0   = mix(0.04, 1.0, refl);
        float NoV  = max(dot(N, V), 0.0);
        float fres = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);
        color = mix(color, env, clamp(fres, 0.0, 1.0));
    }

    // Emission: self-illumination added after lighting/reflection (so it isn't
    // dimmed by them) but before fog (distant glows still haze). An emission map
    // (Unity _Illum) restricts the glow to its lit texels.
    vec3 emissive = pow(uEmission, vec3(2.2)) * uEmissionStrength;
    if (uHasEmissionMap == 1)
        emissive *= pow(texture(uEmissionMap, vUV * uEmissionUVScale).rgb, vec3(2.2));
    // Windows are ADDED, not multiplied in: they are their own light sources, not
    // a mask over the material's glow, so a facade can carry both (neon band and
    // lit storeys). vNormal, not the normal-mapped N -- which wall this is, is a
    // property of the geometry, not of a bump map.
    if (uWindowGrid == 1) emissive += windowEmission(vWorldPos, normalize(vNormal));
    color += emissive;

    color = applyFog(color, vWorldPos, uViewPos, uLightDir);

    // Nothing leaves here that the target cannot hold. The scene renders into an
    // RGBA16F buffer, and a half float stops at 65504: one pixel above that is
    // stored as +Inf, bloom's bright pass then divides that Inf by its own
    // luminance, and the NaN which comes out spreads across every level of the
    // pyramid -- which is exactly what paints blocks of black around anything
    // bright enough (a strong glow, a reflection of one). Clamped at the one
    // point where every contribution has been added up.
    color = min(color, vec3(50000.0));
    if (any(isnan(color))) color = vec3(0.0);

    // Output alpha. Glass modulates it by a Fresnel term: nearly clear when
    // viewed head-on, rising to opaque at grazing angles so the reflective rim
    // (added above via uReflectivity) stays visible -- reads as real glass.
    float outA = uAlpha * texA;
    if (uGlass == 1) {
        float NoV = max(dot(N, V), 0.0);
        float fr  = pow(1.0 - NoV, 5.0);
        outA = mix(uAlpha, 1.0, fr) * texA;
    }
    // Road edge fade: distance (in metres) from the nearest ribbon edge, ramped
    // over uRoadFade. Only active on the road material (uRoadFade > 0).
    if (uRoadFade > 0.0 && uRoadUMax > 0.0) {
        float frac  = clamp(vUV.x / uRoadUMax, 0.0, 1.0); // 0 left .. 1 right edge
        float edgeM = min(frac, 1.0 - frac) * uRoadWidth; // metres from nearest edge
        outA *= clamp(edgeM / uRoadFade, 0.0, 1.0);
    }
    FragColor = vec4(toOutput(color), outA);
}
