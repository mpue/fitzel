#version 330 core

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
out vec4 FragColor;

uniform sampler2D uTex;
uniform int  uAlphaCutout; // 1 for foliage (discard transparent texels)
uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;

uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

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
    vec4 tex = texture(uTex, vUv);
    if (uAlphaCutout == 1 && tex.a < 0.5) discard;

    vec3 albedo = pow(tex.rgb, vec3(2.2));
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    float ndl = dot(N, L);
    // Foliage is translucent -> soft two-sided; bark is opaque -> one-sided so it
    // keeps its form and doesn't read as flat and over-bright.
    float diff = (uAlphaCutout == 1) ? mix(max(ndl, 0.0), abs(ndl), 0.5)
                                     : max(ndl, 0.0);

    vec3 color = albedo * uAmbient * 0.8 + uLightColor * albedo * (diff * 0.85 + 0.05);
    color = applyFog(color, vWorldPos, uViewPos, uLightDir);
    FragColor = vec4(color, 1.0);
}
