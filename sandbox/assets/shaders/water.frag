#version 330 core

in vec4 vClip;
in vec3 vWorldPos;
out vec4 FragColor;

uniform sampler2D uReflection;
uniform sampler2D uRefraction;

uniform vec3  uCameraPos;
uniform vec3  uLightDir;     // towards the light
uniform vec3  uLightColor;
uniform float uTime;
uniform vec3  uWaterColor;
uniform float uWaveStrength; // ripple distortion amount
uniform float uWaveScale;    // ripple frequency

// Atmospheric fog (matches lit.frag).
uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

// --- Value-noise fBm (for ripples) -----------------------------------------
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i),            hash21(i + vec2(1, 0)), u.x),
               mix(hash21(i + vec2(0,1)), hash21(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return s;
}

// Animated surface normal from two scrolling noise layers.
vec3 waterNormal(vec2 p) {
    float t = uTime * 0.06;
    vec2  q = p * uWaveScale;
    float e = 0.35;
    float h  = fbm(q + vec2(t, t * 0.7));
    float hx = fbm(q + vec2(e, 0.0) + vec2(t, t * 0.7));
    float hz = fbm(q + vec2(0.0, e) + vec2(t, t * 0.7));
    return normalize(vec3(h - hx, e * 3.0, h - hz));
}

void main() {
    // Projective UVs from clip-space position. The reflection is rendered with a
    // mirror matrix (view * scale(1,-1,1)), so the texture is already correctly
    // oriented -- both targets sample at the fragment's own screen UV.
    vec2 ndc        = (vClip.xy / vClip.w) * 0.5 + 0.5;
    vec2 refractUV  = ndc;
    vec2 reflectUV  = ndc;

    // Ripple distortion driven by animated noise.
    float t = uTime * 0.04;
    vec2  wp = vWorldPos.xz * uWaveScale;
    vec2  distortion = vec2(fbm(wp + vec2(t, 0.0)),
                            fbm(wp + vec2(0.0, t))) - 0.5;
    distortion *= 2.0 * uWaveStrength;

    refractUV = clamp(refractUV + distortion, 0.002, 0.998);
    reflectUV = clamp(reflectUV + distortion, 0.002, 0.998);

    vec3 reflectCol = texture(uReflection, reflectUV).rgb;
    vec3 refractCol = texture(uRefraction, refractUV).rgb;

    // Fresnel: looking straight down -> see through (refraction); grazing -> mirror.
    vec3  N = waterNormal(vWorldPos.xz);
    vec3  V = normalize(uCameraPos - vWorldPos);
    float fresnel = pow(clamp(dot(V, N), 0.0, 1.0), 0.6);

    // Tint the refracted (underwater) color toward the water color.
    refractCol = mix(refractCol, uWaterColor, 0.4);

    vec3 color = mix(reflectCol, refractCol, fresnel);

    // Specular sun glint off the rippled surface.
    vec3  L = normalize(uLightDir);
    vec3  H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 120.0);
    color += uLightColor * spec * 0.7;

    // Atmospheric fog so distant water blends into the horizon haze.
    vec3  toFrag = vWorldPos - uCameraPos;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(uCameraPos.y - uFogHeight) * b);
    float od = (abs(rd.y) > 1e-4)
             ? c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y)
             : c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));
    float sunAmt = pow(max(dot(rd, normalize(uLightDir)), 0.0), 4.0);
    color = mix(color, mix(uFogColor, uFogSunColor, sunAmt), clamp(fog, 0.0, 1.0));

    FragColor = vec4(color, 1.0);
}
