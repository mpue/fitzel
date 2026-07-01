#version 330 core

in float vH;
in vec3  vWorldPos;
in vec3  vNormal;
in float vLush;
out vec4 FragColor;

uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;
uniform vec3 uTint;       // grass colour multiplier

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
    // Dry (straw/khaki) vs lush (green) grass by biome moisture.
    vec3 baseCol = mix(vec3(0.14, 0.13, 0.06), vec3(0.05, 0.15, 0.03), vLush);
    vec3 tipCol  = mix(vec3(0.46, 0.42, 0.20), vec3(0.30, 0.50, 0.12), vLush);

    // Distance flattening: blend each blade toward the mean colour with range so
    // far-field blades don't shimmer into noise.
    float dist    = length(vWorldPos - uViewPos);
    float farFade = smoothstep(8.0, 38.0, dist);
    float h       = mix(vH, 0.6, farFade);
    vec3 albedo = pow(mix(baseCol, tipCol, h), vec3(2.2)) * uTint; // -> linear

    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    float diff = max(dot(N, L), 0.0);
    float back = max(dot(-N, L), 0.0) * 0.3; // light through the blade

    vec3 color = albedo * uAmbient
               + uLightColor * albedo * (diff * 0.8 + 0.2 + back);
    color *= mix(mix(0.6, 1.0, vH), 1.0, farFade); // softer base AO, none at distance

    color = applyFog(color, vWorldPos, uViewPos, uLightDir);
    FragColor = vec4(color, 1.0);
}
