#version 330 core

#define MAX_CASCADES 4

in vec3  vWorldPos;
in vec3  vNormal;
in vec2  vUV;
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
uniform vec3 uAlbedo;
uniform float uAlpha;         // material opacity (1 = opaque); * texture alpha
uniform int   uGlass;         // 1 = Fresnel alpha (clear head-on, opaque rim)

// Terrain palette (uColorMode == 1).
uniform vec3  uColorSand;
uniform vec3  uColorGrass;
uniform vec3  uColorRock;
uniform vec3  uColorSnow;
uniform float uSnowLevel;      // world height at which snow appears
uniform float uRockSlope;      // slope (normal.y) below which faces turn to rock
uniform float uSlopeSharpness; // blend width of the rock transition

// Triplanar terrain textures (uColorMode == 1).
uniform sampler2D uTexSand;    // low ground near water
uniform sampler2D uTexGround;  // flat ground / rocky soil
uniform sampler2D uTexCliff;   // steep rock
uniform sampler2D uTexSnow;    // high, flat snow
uniform float     uTexScale;   // world units -> texture tiling
uniform float     uSandLevel;  // height below which sand dominates

// Matching triplanar normal maps (OpenGL convention).
uniform sampler2D uTexSandN;
uniform sampler2D uTexGroundN;
uniform sampler2D uTexCliffN;
uniform sampler2D uTexSnowN;
uniform float     uNormalStrength; // 0 = geometry normal, 1 = full normal map
uniform float     uWaterLevel;     // surfaces below this are wet (darker)

// Procedural micro-detail (uColorMode == 1).
uniform float uDetailScale;    // frequency of the close-up detail
uniform float uDetailStrength; // how strongly it perturbs the normal

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
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closest = texture(uShadowMap,
                                    vec3(proj.xy + vec2(x, y) * texel, float(layer))).r;
            shadow += (current - bias > closest) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
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

// Normal-mapped surface normal, blended across the same material masks as the
// albedo (masks derived from the smooth geometry normal `n`).
vec3 terrainNormal(vec3 wp, vec3 n, float detail) {
    float flatness = smoothstep(uRockSlope - uSlopeSharpness,
                                uRockSlope + uSlopeSharpness, n.y);
    vec3 sandN   = triplanarNormal(uTexSandN,   wp, n, uTexScale * 1.4);
    vec3 groundN = triplanarNormal(uTexGroundN, wp, n, uTexScale);
    vec3 cliffN  = triplanarNormal(uTexCliffN,  wp, n, uTexScale * 0.7);
    vec3 snowN   = triplanarNormal(uTexSnowN,   wp, n, uTexScale);

    float sandH     = uSandLevel + (detail - 0.5) * 3.0;
    float groundMix = smoothstep(sandH, sandH + 5.0, wp.y);
    vec3  lowN      = mix(sandN, groundN, groundMix);

    float snowH    = uSnowLevel + (detail - 0.5) * 6.0;
    float snowMask = smoothstep(snowH - 2.5, snowH + 2.5, wp.y) * flatness;

    vec3 baseN = mix(cliffN, lowN, flatness);
    baseN = mix(baseN, snowN, snowMask);
    return normalize(mix(n, baseN, uNormalStrength));
}

// Height- and slope-based terrain albedo from three triplanar textures.
// `slope` is the upward component of the surface normal: 1 = flat, ~0 = vertical.
vec3 terrainAlbedo(vec3 wp, vec3 n, float detail) {
    float flatness = smoothstep(uRockSlope - uSlopeSharpness,
                                uRockSlope + uSlopeSharpness, n.y);

    vec3 sand   = triplanar(uTexSand,   wp, n, uTexScale * 1.4);
    vec3 ground = triplanar(uTexGround, wp, n, uTexScale);
    vec3 cliff  = triplanar(uTexCliff,  wp, n, uTexScale * 0.7);
    vec3 snow   = triplanar(uTexSnow,   wp, n, uTexScale);

    // Sand near the water line, blending up into rocky ground.
    float sandH      = uSandLevel + (detail - 0.5) * 3.0;
    float groundMix  = smoothstep(sandH, sandH + 5.0, wp.y);
    vec3  lowGround  = mix(sand, ground, groundMix);

    // Break the snow line with noise so it isn't a hard contour.
    float snowH    = uSnowLevel + (detail - 0.5) * 6.0;
    float snowMask = smoothstep(snowH - 2.5, snowH + 2.5, wp.y) * flatness;

    vec3 base = mix(cliff, lowGround, flatness);
    return mix(base, snow, snowMask);
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

void main() {
    vec3 N = normalize(vNormal); // smooth geometry normal (drives material masks)

    float detail = (uColorMode == 1) ? detailFbm(vWorldPos.xz * uDetailScale) : 0.0;

    vec3 albedo;
    float texA = 1.0; // texture alpha, folded into the output alpha
    if (uColorMode == 1) {
        albedo = terrainAlbedo(vWorldPos, N, detail);
    } else if (uColorMode == 2) {
        vec4 t = texture(uTexture, vUV);
        albedo = t.rgb; texA = t.a;
    } else {
        albedo = uAlbedo;
    }
    albedo = pow(albedo, vec3(2.2)); // sRGB -> linear for correct lighting

    // Submerged (and just-above-waterline) surfaces are wet -> darker, with a
    // narrow damp band reaching slightly above the surface.
    float wet = 1.0 - smoothstep(uWaterLevel - 0.8, uWaterLevel + 0.4, vWorldPos.y);
    albedo *= mix(1.0, 0.42, wet);

    // Detailed normal from the triplanar normal maps, for lighting.
    if (uColorMode == 1) {
        N = terrainNormal(vWorldPos, N, detail);
    }

    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 H = normalize(L + V);

    float diff      = max(dot(N, L), 0.0);
    // Terrain: subtle. Textured surfaces (roads): rough/matte, faint broad sheen.
    float specPower = (uColorMode == 1) ? 0.15 : 0.03;
    float specExp   = (uColorMode == 1) ? 48.0 : 14.0;
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
    FragColor = vec4(toOutput(color), outA);
}
