#version 330 core

// The tree generator's preview. The lighting is tree.frag's -- soft two-sided
// foliage, the sun through the leaves, sky above and ground below -- so the
// studio shows the tree as the forest will. What it adds is its own sun shadow
// (one map, PCF) and a ground patch to catch it.
in vec3 vPos;
in vec3 vNormal;
in vec2 vUv;
out vec4 FragColor;

uniform sampler2D uTex;
uniform sampler2D uShadow;
uniform mat4  uLightVP;
uniform int   uLeaf;       // 1: alpha-cut foliage
uniform int   uGround;     // 1: the ground patch
uniform float uGroundR;
uniform vec3  uSun;        // towards the sun
uniform vec3  uEye;
uniform vec3  uSky;        // background, sRGB
uniform sampler2D uNormalTex;  // the bark's relief (glTF convention), unit 2
uniform int   uHasNormal;
uniform float uNormalStrength;

#include "treenormal.glsl"

float sunShadow(vec3 p, vec3 n) {
    vec4 l = uLightVP * vec4(p + n * 0.03, 1.0);
    vec3 c = l.xyz / l.w * 0.5 + 0.5;
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadow, 0));
    float lit = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            float d = texture(uShadow, c.xy + vec2(x, y) * texel).r;
            lit += (c.z - 0.0015 <= d) ? 1.0 : 0.0;
        }
    return lit / 25.0;
}

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec4 tex = texture(uTex, vUv);
    if (uGround == 1) {
        float n = fract(sin(dot(floor(vPos.xz / uGroundR * 90.0), vec2(12.9898, 78.233))) * 43758.5453);
        tex = vec4(mix(vec3(0.20, 0.25, 0.11), vec3(0.26, 0.30, 0.14), n), 1.0);
    }
    if (uLeaf == 1 && tex.a < 0.5) discard;

    vec3 albedo = pow(tex.rgb, vec3(2.2));
    vec3 N = normalize(vNormal);
    if (uHasNormal == 1) N = treeNormalMap(uNormalTex, N, vPos, vUv, uNormalStrength);
    vec3 L = normalize(uSun);
    float ndl = dot(N, L);
    float diff = max(ndl, 0.0);
    vec3 lit = albedo * diff;
    if (uLeaf == 1) {
        float peak = max(max(albedo.r, albedo.g), max(albedo.b, 1e-4));
        lit = mix(lit, albedo * diff + albedo * (albedo / peak) * max(-ndl, 0.0), 0.5);
    }
    float sun = sunShadow(vPos, normalize(vNormal));   // offset along the surface, not the relief
    // The engine's midday (main.cpp: sun 3.4 x day, ambient 0.12/0.14/0.18),
    // the ambient lifted a little for the sky light the forest gets from IBL.
    vec3 sunCol = vec3(1.0, 0.95, 0.88) * 3.2;
    vec3 skyAmb = vec3(0.16, 0.19, 0.25);
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 amb = skyAmb * mix(0.45, 1.0, up);
    vec3 color = albedo * amb * 0.8 + sunCol * (lit * 0.85 + albedo * 0.05) * sun;
    if (uLeaf == 1) {
        vec3 V = normalize(uEye - vPos);
        float back = pow(max(dot(V, -L), 0.0), 4.0);
        float thin = 0.35 + 0.65 * max(-ndl, 0.0);
        color += sunCol * albedo * albedo * (back * thin * 1.6) * sun;
    }
    color = pow(aces(color * 0.75), vec3(1.0 / 2.2));
    if (uGround == 1) {
        float edge = smoothstep(0.75, 1.0, length(vPos.xz) / uGroundR);
        color = mix(color, uSky, edge);
    }
    FragColor = vec4(color, 1.0);
}