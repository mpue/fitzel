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

float computeShadow(int layer, vec3 N, vec3 L) {
    vec4 lsPos = uLightSpace[layer] * vec4(vWorldPos, 1.0);
    vec3 proj  = lsPos.xyz / lsPos.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) return 0.0;

    // Kept small: glPolygonOffset in the depth pass already handles most acne, so
    // a large bias here only detaches the shadow from the caster (peter-panning).
    float bias = max(0.0010 * (1.0 - dot(N, L)), 0.00025);
    bias *= 1.0 + float(layer) * 0.35; // coarser cascades need a little more

    vec2  texel   = 1.0 / vec2(textureSize(uShadowMap, 0).xy);
    float current = proj.z;
    float shadow  = 0.0;
    // Wider 5x5 PCF at 1.35-texel spacing: a broader, softer penumbra so cast
    // shadows (e.g. a tree on the meadow) fade out instead of ending on a hard
    // edge. Nearer cascades cover less ground per texel, so their edge is softest.
    const float spread = 1.35;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture(uShadowMap,
                vec3(proj.xy + vec2(x, y) * texel * spread, float(layer))).r;
            shadow += (current - bias > closest) ? 1.0 : 0.0;
        }
    }
    return shadow / 25.0;
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

// Triplanar sampling: project the texture along the three world axes and blend
// by the (squared) normal, so steep terrain doesn't stretch a flat UV.
vec3 triplanar(sampler2D tex, vec3 wp, vec3 n, float scale) {
    vec3 bw = abs(n);
    bw = pow(bw, vec3(4.0));
    bw /= (bw.x + bw.y + bw.z);
    vec3 cx = texture(tex, wp.zy * scale).rgb;
    vec3 cy = texture(tex, wp.xz * scale).rgb;
    vec3 cz = texture(tex, wp.xy * scale).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}

// Triplanar normal mapping (Whiteout blend): reorient each plane's tangent-space
// normal onto the geometry normal, then blend by the (squared) normal.
vec3 triplanarNormal(sampler2D nmap, vec3 wp, vec3 n, float scale) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= (bw.x + bw.y + bw.z);
    vec3 tx = texture(nmap, wp.zy * scale).xyz * 2.0 - 1.0;
    vec3 ty = texture(nmap, wp.xz * scale).xyz * 2.0 - 1.0;
    vec3 tz = texture(nmap, wp.xy * scale).xyz * 2.0 - 1.0;
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
vec3 layerTriplanar(int i, vec3 wp, vec3 n, float scale) {
    if (i == 0) return triplanar(uLayerTex[0], wp, n, scale);
    if (i == 1) return triplanar(uLayerTex[1], wp, n, scale);
    if (i == 2) return triplanar(uLayerTex[2], wp, n, scale);
    if (i == 3) return triplanar(uLayerTex[3], wp, n, scale);
    if (i == 4) return triplanar(uLayerTex[4], wp, n, scale);
    return triplanar(uLayerTex[5], wp, n, scale);
}

// Same constant-index dispatch for the per-layer normal maps.
vec3 layerTriplanarNormal(int i, vec3 wp, vec3 n, float scale) {
    if (i == 0) return triplanarNormal(uLayerNorm[0], wp, n, scale);
    if (i == 1) return triplanarNormal(uLayerNorm[1], wp, n, scale);
    if (i == 2) return triplanarNormal(uLayerNorm[2], wp, n, scale);
    if (i == 3) return triplanarNormal(uLayerNorm[3], wp, n, scale);
    if (i == 4) return triplanarNormal(uLayerNorm[4], wp, n, scale);
    return triplanarNormal(uLayerNorm[5], wp, n, scale);
}

// Height- and slope-driven terrain surface: blends the configured texture layers
// by coverage, producing both the albedo and the perturbed normal in one pass.
// Layer coverage (height band x slope band) weights both, so a layer's normal
// map only shows where its texture shows. uNormalStrength dials the relief in.
void terrainSurface(vec3 wp, vec3 n, float detail, out vec3 albedo, out vec3 normalOut) {
    normalOut = n;
    if (uLayerCount == 0) { albedo = uAlbedo; return; } // no layers -> flat base

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
            acc += layerTriplanar(i, wp, n, uLayerScale[i]) * w;
            vec3 ln = (uLayerHasNorm[i] == 1)
                    ? layerTriplanarNormal(i, wp, n, uLayerScale[i]) : n;
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

    float detail = (uColorMode == 1) ? detailFbm(vWorldPos.xz * uDetailScale) : 0.0;

    vec3 albedo;
    vec3 terrainNrm = N; // terrain's perturbed normal (filled in layer mode)
    float texA = 1.0; // texture alpha, folded into the output alpha
    if (uColorMode == 1) {
        terrainSurface(vWorldPos, N, detail, albedo, terrainNrm);
    } else if (uColorMode == 2) {
        vec4 t = texture(uTexture, vUV);
        albedo = t.rgb; texA = t.a;
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

    // Drops striking the surface. Before the wetness below, so the rings catch the
    // wet sheen too -- that glinting is most of what sells them; the geometry never
    // moves. Gated on the up-facing normal like the wetness itself.
    if (uRainRings > 0.002)
        N = rainRings(N, vWorldPos.xz, uRainRings * clamp(N.y * 1.3, 0.0, 1.0),
                      uTime);

    // Rain wetness: sky-exposed (up-facing) surfaces darken and turn glossy while
    // it rains. Gated by the up-facing normal so walls/undersides stay dry.
    float rainWet = uWetness * clamp(N.y * 1.3, 0.0, 1.0);
    albedo *= mix(1.0, 0.6, rainWet);

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
    float spec      = pow(max(dot(N, H), 0.0), specExp) * specPower;

    int   layer   = selectCascade();
    float shadow  = computeShadow(layer, N, L);

    // Ambient: image-based lighting from the HDRI when enabled, else the flat
    // directional ambient. IBL gives diffuse irradiance + a soft env specular.
    vec3 ambient;
    if (uUseIBL == 1) {
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

    // Environment reflection: sample the dynamic scene probe along the reflection
    // vector and blend in by a Fresnel term. uReflectivity raises the base
    // reflectance F0 (0 -> dielectric 4%, 1 -> full mirror); uRoughness selects a
    // blurrier mip. Reflection happens before fog so distant mirrors haze too.
    if (uReflectivity > 0.0) {
        vec3  Rv   = reflect(-V, N);
        vec3  env  = textureLod(uEnvProbe, Rv, uRoughness * uEnvMaxLod).rgb;
        float F0   = mix(0.04, 1.0, uReflectivity);
        float NoV  = max(dot(N, V), 0.0);
        float fres = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);
        color = mix(color, env, clamp(fres, 0.0, 1.0));
    }

    // Emission: self-illumination added after lighting/reflection (so it isn't
    // dimmed by them) but before fog (distant glows still haze). An emission map
    // (Unity _Illum) restricts the glow to its lit texels.
    vec3 emissive = pow(uEmission, vec3(2.2)) * uEmissionStrength;
    if (uHasEmissionMap == 1)
        emissive *= pow(texture(uEmissionMap, vUV).rgb, vec3(2.2));
    color += emissive;

    color = applyFog(color, vWorldPos, uViewPos, uLightDir);

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
