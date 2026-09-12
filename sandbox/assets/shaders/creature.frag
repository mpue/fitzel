#version 330 core

in vec3  vWorldPos;
in vec2  vUv;
in vec3  vColor;
in float vBelly;
in vec3  vUp;
in float vWing;
out vec4 FragColor;

uniform int  uKind;            // 0 bird, 1 butterfly
uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;
uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

#include "sunshadow.glsl"

vec3 applyFog(vec3 color, vec3 worldPos, vec3 eye, vec3 lightDir) {
    vec3  toFrag = worldPos - eye;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(eye.y - uFogHeight) * b);
    float od = (abs(rd.y) > 1e-4) ? c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y) : c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));
    float sunAmt = pow(max(dot(rd, normalize(lightDir)), 0.0), 4.0);
    return mix(color, mix(uFogColor, uFogSunColor, sunAmt), clamp(fog, 0.0, 1.0));
}

void main() {
    vec3 V = normalize(uViewPos - vWorldPos);
    // The face as seen: flat, turned towards the eye. Whether that face is the
    // bird's back or its belly decides which colour it shows.
    vec3 N = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
    if (dot(N, V) < 0.0) N = -N;
    bool belly = dot(N, vUp) < 0.0;

    vec3 col = vColor;
    if (uKind == 1 && vWing > 0.03) {
        // Cut each wing quad into a wing and paint it: a dark rim, veins
        // fanning from the root, and on the fore wing an eyespot.
        bool hind = vUv.x >= 2.0;
        vec2 uv   = vec2(vUv.x - (hind ? 2.0 : 0.0), vUv.y);
        vec2 c    = hind ? vec2(0.40, 0.50) : vec2(0.48, 0.56);
        float d   = length((uv - c) * vec2(1.0, 1.2));
        if (d > 0.60) discard;
        float rim  = smoothstep(0.40, 0.55, d);
        float ang  = atan(uv.y - 0.5, uv.x + 0.1);
        float vein = smoothstep(0.90, 1.0, abs(sin(ang * 7.0))) * smoothstep(0.1, 0.3, uv.x);
        col = mix(col, col * 0.12, rim * 0.85);
        col = mix(col, col * 0.3, vein * 0.5);
        if (!hind) {
            float spot = 1.0 - smoothstep(0.05, 0.08, length(uv - vec2(0.70, 0.66)));
            float ring = 1.0 - smoothstep(0.08, 0.11, length(uv - vec2(0.70, 0.66)));
            col = mix(col, vec3(0.08), ring * 0.8);
            col = mix(col, vec3(0.85, 0.85, 0.9), spot * 0.9);
        }
        // Undersides of butterfly wings are the dull, camouflaged side.
        if (belly) col = mix(col, vec3(0.35, 0.3, 0.22), 0.55);
    } else if (belly) {
        col = mix(col, vec3(0.82, 0.76, 0.66), vBelly);
    }
    vec3 albedo = pow(clamp(col, 0.0, 1.0), vec3(2.2));

    vec3  L   = normalize(uLightDir);
    float ndl = dot(N, L);
    float sun = 1.0 - sunShadow(vWorldPos, L, 2.0);
    // Thin things: light through the wing when the sun is behind it.
    float through = max(-ndl, 0.0) * (uKind == 1 ? 0.6 : 0.25);
    vec3  amb   = uAmbient * (0.55 + 0.45 * abs(N.y));
    vec3  color = albedo * (amb + uLightColor * (max(ndl, 0.0) * 0.9 + through) * sun);
    color = applyFog(color, vWorldPos, uViewPos, L);
    FragColor = vec4(color, 1.0);
}
