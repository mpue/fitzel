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

// Shadows (cascaded).
uniform sampler2DArray uShadowMap;
uniform mat4  uLightSpace[MAX_CASCADES];
uniform float uCascadeSplits[MAX_CASCADES];
uniform int   uCascadeCount;

// Surface.
uniform sampler2D uTexture;   // used when uColorMode == 2
uniform int  uColorMode;      // 0 = uAlbedo, 1 = terrain palette, 2 = texture
uniform vec3 uAlbedo;

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

// Height- and slope-based palette (sand -> grass -> rock -> snow).
vec3 terrainColor(float height, float slope) {
    vec3 sand  = vec3(0.76, 0.70, 0.48);
    vec3 grass = vec3(0.23, 0.42, 0.16);
    vec3 rock  = vec3(0.38, 0.34, 0.30);
    vec3 snow  = vec3(0.92, 0.94, 0.98);

    vec3 c = mix(sand, grass, smoothstep(-3.0, 2.0, height));
    c = mix(c, snow, smoothstep(7.0, 11.0, height));
    c = mix(rock, c, smoothstep(0.45, 0.78, slope)); // steep faces -> rock
    return c;
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 H = normalize(L + V);

    vec3 albedo;
    if (uColorMode == 1)      albedo = terrainColor(vWorldPos.y, N.y);
    else if (uColorMode == 2) albedo = texture(uTexture, vUV).rgb;
    else                      albedo = uAlbedo;

    float diff      = max(dot(N, L), 0.0);
    float specPower = (uColorMode == 1) ? 0.15 : 0.5;
    float spec      = pow(max(dot(N, H), 0.0), 48.0) * specPower;

    int   layer   = selectCascade();
    float shadow  = computeShadow(layer, N, L);
    float ambient = 0.30;

    vec3 color = albedo * ambient
               + (1.0 - shadow) * uLightColor * (albedo * diff + spec);

    FragColor = vec4(color, 1.0);
}
