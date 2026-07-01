#version 330 core

in float vH;
in vec3  vWorldPos;
in vec3  vNormal;
in float vLush;
in float vRand;
out vec4 FragColor;

// Cheap 2D value noise for large-scale colour patches across the meadow.
float ghash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float gnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = ghash(i), b = ghash(i + vec2(1, 0));
    float c = ghash(i + vec2(0, 1)), d = ghash(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

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
    // Colour variation: large meadow patches + a finer break + per-blade jitter,
    // so the field mixes lush green, olive and dry straw instead of one flat green.
    float patch = gnoise(vWorldPos.xz * 0.05);
    float fine  = gnoise(vWorldPos.xz * 0.27 + 11.0);
    // "green-ness": biome moisture, pulled down in some patches/blades toward dry.
    float green = clamp(vLush - (1.0 - patch) * 0.55 + (fine - 0.5) * 0.3
                              - vRand * 0.25, 0.0, 1.0);

    vec3 dryBase = vec3(0.17, 0.15, 0.07);
    vec3 dryTip  = vec3(0.55, 0.48, 0.24);
    vec3 lushBase = mix(vec3(0.05, 0.13, 0.03), vec3(0.09, 0.19, 0.05), fine);
    vec3 lushTip  = mix(vec3(0.20, 0.40, 0.10), vec3(0.42, 0.55, 0.16), fine);
    vec3 baseCol = mix(dryBase, lushBase, green);
    vec3 tipCol  = mix(dryTip,  lushTip,  green);
    // Per-blade brightness so neighbours don't read identically.
    float bright = 0.80 + 0.40 * vRand;
    baseCol *= bright;
    tipCol  *= bright;

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
