#version 330 core

in vec2  vUv;
in vec3  vWorldPos;
in vec3  vRight;
in vec3  vUp;
in vec3  vFwd;
in float vFade;
in float vFlip;
out vec4 FragColor;

uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;

uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

uniform float uBrightness;
uniform float uContrast;
uniform float uHue;

#include "sunshadow.glsl"
#include "treedither.glsl"

vec3 hueShift(vec3 col, float a) {
    const vec3 k = vec3(0.57735026919);
    float c = cos(a), s = sin(a);
    return col * c + cross(k, col) * s + k * dot(k, col) * (1.0 - c);
}
vec3 correct(vec3 c) {
    c = hueShift(c, uHue);
    c = (c - 0.5) * uContrast + 0.5;
    c *= uBrightness;
    return clamp(c, 0.0, 1.0);
}

vec3 applyFog(vec3 color, vec3 worldPos, vec3 eye, vec3 lightDir) {
    vec3  toFrag = worldPos - eye;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(eye.y - uFogHeight) * b);
    float od = (abs(rd.y) > 1e-4)
             ? c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y)
             : c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));
    float sunAmt = pow(max(dot(rd, normalize(lightDir)), 0.0), 4.0);
    return mix(color, mix(uFogColor, uFogSunColor, sunAmt), clamp(fog, 0.0, 1.0));
}

void main() {
    vec4 a = texture(uAlbedo, vUv);
    if (a.a < 0.5) discard;
    // The mesh keeps this pixel while it fades out; the card takes the rest.
    if (!treeDitherKeep(1.0 - vFade, true)) discard;

    vec4 nb = texture(uNormal, vUv);
    vec3 n  = nb.xyz * 2.0 - 1.0;
    if (vFlip > 0.5) n.x = -n.x;
    vec3 N  = normalize(vRight * n.x + vUp * n.y + vFwd * max(n.z, 0.05));
    bool leaf = nb.a > 0.75;

    vec3  albedo = pow(correct(a.rgb), vec3(2.2));
    vec3  L   = normalize(uLightDir);
    float ndl = dot(N, L);
    float diff = leaf ? mix(max(ndl, 0.0), abs(ndl), 0.5) : max(ndl, 0.0);
    float sun = 1.0 - sunShadow(vWorldPos, L, 4.0);
    float up  = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  amb = uAmbient * mix(0.45, 1.0, up);
    vec3 color = albedo * amb * 0.8 + uLightColor * albedo * (diff * 0.85 + 0.05) * sun;
    if (leaf) {
        vec3  V    = normalize(uViewPos - vWorldPos);
        float back = pow(max(dot(V, -L), 0.0), 4.0);
        float thin = 0.35 + 0.65 * max(-ndl, 0.0);
        color += uLightColor * albedo * albedo * (back * thin * 1.6) * sun;
    }
    color = applyFog(color, vWorldPos, uViewPos, uLightDir);
    FragColor = vec4(color, 1.0);
}
