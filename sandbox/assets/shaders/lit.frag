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

// Shadows (cascaded).
uniform sampler2DArray uShadowMap;
uniform mat4  uLightSpace[MAX_CASCADES];
uniform float uCascadeSplits[MAX_CASCADES];
uniform int   uCascadeCount;

// Surface.
uniform sampler2D uTexture;   // used when uColorMode == 2
uniform int  uColorMode;      // 0 = uAlbedo, 1 = terrain palette, 2 = texture
uniform vec3 uAlbedo;

// Terrain palette (uColorMode == 1).
uniform vec3  uColorSand;
uniform vec3  uColorGrass;
uniform vec3  uColorRock;
uniform vec3  uColorSnow;
uniform float uSnowLevel;      // world height at which snow appears
uniform float uRockSlope;      // slope (normal.y) below which faces turn to rock
uniform float uSlopeSharpness; // blend width of the rock transition

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

    float bias = max(0.0025 * (1.0 - dot(N, L)), 0.0008);
    bias *= 1.0 + float(layer) * 0.6; // coarser cascades need more bias

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

// Height- and slope-based palette. `slope` is the upward component of the
// surface normal: 1.0 = flat ground, ~0.0 = a vertical face.
vec3 terrainColor(float height, float slope) {
    // 0 on steep faces -> 1 on flat ground.
    float flatness = smoothstep(uRockSlope - uSlopeSharpness,
                                uRockSlope + uSlopeSharpness, slope);

    // Base ground by height.
    vec3 ground = mix(uColorSand, uColorGrass, smoothstep(-3.0, 2.0, height));

    // Snow settles on high ground, but only where it's flat enough to hold.
    float snowMask = smoothstep(uSnowLevel - 2.0, uSnowLevel + 2.0, height) * flatness;
    ground = mix(ground, uColorSnow, snowMask);

    // Steep faces are bare rock regardless of height.
    return mix(uColorRock, ground, flatness);
}

void main() {
    vec3 N = normalize(vNormal);

    // Add fine surface detail to the terrain: perturb the normal with a
    // high-frequency noise gradient (bump mapping) and break up the albedo.
    float detail = 0.0;
    if (uColorMode == 1) {
        vec2  p  = vWorldPos.xz * uDetailScale;
        float e  = 0.75;
        detail   = detailFbm(p);
        float dX = detailFbm(p + vec2(e, 0.0)) - detail;
        float dZ = detailFbm(p + vec2(0.0, e)) - detail;
        N = normalize(N - vec3(dX, 0.0, dZ) * uDetailStrength);
    }

    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 H = normalize(L + V);

    vec3 albedo;
    if (uColorMode == 1) {
        albedo = terrainColor(vWorldPos.y, N.y);
        albedo *= 0.88 + 0.24 * detail; // subtle tonal variation
    } else if (uColorMode == 2) {
        albedo = texture(uTexture, vUV).rgb;
    } else {
        albedo = uAlbedo;
    }

    float diff      = max(dot(N, L), 0.0);
    float specPower = (uColorMode == 1) ? 0.15 : 0.5;
    float spec      = pow(max(dot(N, H), 0.0), 48.0) * specPower;

    int   layer   = selectCascade();
    float shadow  = computeShadow(layer, N, L);

    vec3 color = albedo * uAmbient
               + (1.0 - shadow) * uLightColor * (albedo * diff + spec);

    FragColor = vec4(color, 1.0);
}
