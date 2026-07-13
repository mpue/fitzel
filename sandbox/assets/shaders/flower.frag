#version 330 core

in vec3  vWorldPos;
in vec3  vNormal;
in vec3  vColor;
in float vTint;
out vec4 FragColor;

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
    // Part select: 0 = stem (green), 1 = petal (instance colour), 2 = centre.
    vec3 base;
    if (vTint < 0.5)      base = vec3(0.11, 0.20, 0.06); // stem: lighter green, not near-black
    else if (vTint < 1.5) base = vColor;
    else                  base = vec3(0.96, 0.82, 0.16);
    vec3 albedo = pow(base, vec3(2.2)); // sRGB -> linear

    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    float diff = max(dot(N, L), 0.0);
    float back = max(dot(-N, L), 0.0) * 0.3; // light through thin petals

    vec3 color = albedo * uAmbient
               + uLightColor * albedo * (diff * 0.8 + 0.25 + back);
    color = applyFog(color, vWorldPos, uViewPos, uLightDir);
    FragColor = vec4(color, 1.0);
}
