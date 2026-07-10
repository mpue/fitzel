#version 330 core

in vec4  vClip;
in vec3  vWorldPos;
in vec3  vWaveNormal;
in float vCrest;
out vec4 FragColor;

uniform sampler2D uReflection;
uniform sampler2D uRefraction;
uniform sampler2D uRefractionDepth; // scene depth behind the water
uniform float uNear;
uniform float uFar;
uniform float uFoamWidth; // world-ish depth over which shoreline foam fades

uniform vec3  uCameraPos;
uniform vec3  uLightDir;     // towards the light
uniform vec3  uLightColor;
uniform vec3  uAmbient;      // sky/fill light (for lit foam)
uniform float uTime;
uniform vec3  uWaterColor;
uniform float uWaveStrength; // ripple distortion amount
uniform float uWaveScale;    // ripple frequency
uniform float uReflectivity; // max mirror strength (Fresnel cap), 0..1
uniform float uClarity;      // water clarity: higher = clearer (less depth tint)
uniform float uIor;          // index of refraction (water ~1.33)

// Atmospheric fog (matches lit.frag).
uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

uniform float uExposure;
uniform int   uTonemap;

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 toOutput(vec3 c) {
    if (uTonemap == 1) {
        c = acesTonemap(c * uExposure);
        c = pow(c, vec3(1.0 / 2.2));
    }
    return c;
}

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

// Window-space depth [0,1] -> linear eye distance.
float linearDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

// Animated surface normal from two scrolling noise layers at different scales
// and directions -> richer, less uniform ripples.
vec3 waterNormal(vec2 p) {
    float e = 0.30;
    // Three scrolling octaves at different scales/directions -- the fine third
    // layer breaks up the reflection so the surface reads as water, not a mirror.
    vec2 q1 = p * uWaveScale       + vec2(uTime * 0.05,  uTime * 0.035);
    vec2 q2 = p * uWaveScale * 2.7 - vec2(uTime * 0.028, uTime * 0.06);
    vec2 q3 = p * uWaveScale * 6.1 + vec2(uTime * 0.09, -uTime * 0.05);
    float h  = fbm(q1)              + 0.5 * fbm(q2)              + 0.25 * fbm(q3);
    float hx = fbm(q1 + vec2(e, 0)) + 0.5 * fbm(q2 + vec2(e, 0)) + 0.25 * fbm(q3 + vec2(e, 0));
    float hz = fbm(q1 + vec2(0, e)) + 0.5 * fbm(q2 + vec2(0, e)) + 0.25 * fbm(q3 + vec2(0, e));
    return normalize(vec3(h - hx, e * 2.0, h - hz));
}

void main() {
    // Projective UVs from clip-space position. The reflection is rendered with a
    // mirror matrix (view * scale(1,-1,1)), so the texture is already correctly
    // oriented -- both targets sample at the fragment's own screen UV.
    vec2 ndc = (vClip.xy / vClip.w) * 0.5 + 0.5;

    // Combine the large Gerstner swell normal with fine noise ripples. The ripple
    // is weighted up a touch so the reflection breaks rather than mirroring.
    vec3  ripple = waterNormal(vWorldPos.xz);
    vec3  N = normalize(vWaveNormal + vec3(ripple.x, 0.0, ripple.z) * 1.4);
    vec3  V = normalize(uCameraPos - vWorldPos);

    // Water column thickness = scene depth behind the water minus the surface
    // depth. Sampled at the *undistorted* screen position so it tracks the real
    // shoreline. `shallow` runs 0 at the waterline -> 1 in open water.
    float sceneZ = linearDepth(texture(uRefractionDepth, ndc).r);
    float waterZ = linearDepth(gl_FragCoord.z);
    float thickness = max(sceneZ - waterZ, 0.0);
    float shallow   = smoothstep(0.0, 1.2, thickness);

    // Ripple distortion, damped in the shallows so the near-shore bed stays
    // readable (no smeared, hard edge). The apparent refraction scales with the
    // air->water index contrast, normalised so water's 1.33 matches the tuned look.
    float bend = (uIor - 1.0) / 0.33;
    vec2 distortion = N.xz * uWaveStrength * shallow * bend;
    vec2 refractUV  = clamp(ndc + distortion, 0.002, 0.998);
    vec2 reflectUV  = clamp(ndc + distortion, 0.002, 0.998);

    vec3 reflectCol = texture(uReflection, reflectUV).rgb;
    vec3 refractCol = texture(uRefraction, refractUV).rgb;

    // Beer-Lambert depth absorption: clear over a shallow bed, tinting toward the
    // water colour with depth. Red is absorbed fastest -> natural blue-green.
    // `uClarity` scales the extinction (higher = clearer water, less tint).
    vec3  absorb = vec3(0.30, 0.14, 0.10) / max(uClarity, 0.05);
    vec3  trans  = exp(-absorb * thickness);
    vec3  deep   = pow(uWaterColor, vec3(2.2));
    vec3  body   = mix(deep, refractCol, trans); // shallow -> bed, deep -> tint

    // Schlick Fresnel with the reflectance-at-normal-incidence derived from the
    // index of refraction: F0 = ((1-n)/(1+n))^2  (~0.02 for water's 1.33). Never a
    // perfect mirror (capped), and reduced over shallow water where you'd see the
    // bed, not the sky -- kills the oily, glass-like look.
    float cosT = clamp(dot(V, N), 0.0, 1.0);
    float f0 = (1.0 - uIor) / (1.0 + uIor);
    f0 *= f0;
    float F = f0 + (1.0 - f0) * pow(1.0 - cosT, 5.0);
    F = min(F, uReflectivity) * mix(0.35, 1.0, shallow);

    vec3 color = mix(body, reflectCol, F);

    // Sharp specular sun glint (HDR, picked up by bloom).
    vec3  L = normalize(uLightDir);
    vec3  H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 300.0);
    color += uLightColor * spec * 3.0;

    // Shoreline foam: a thin, soft band where the water is shallow (no hard line).
    float shoreF = 1.0 - smoothstep(0.0, uFoamWidth, thickness);
    float fn     = fbm(vWorldPos.xz * 0.6 + vec2(uTime * 0.08, -uTime * 0.05));
    shoreF       = smoothstep(0.30, 0.85, shoreF) * (0.4 + 0.6 * fn);

    // Whitecaps / spray on the steep wave crests.
    float capNoise = fbm(vWorldPos.xz * 0.35 - vec2(uTime * 0.06));
    float whitecap = smoothstep(0.60, 0.92, vCrest) * smoothstep(0.40, 0.85, capNoise);

    float foam = clamp(max(shoreF, whitecap * 0.7), 0.0, 1.0);

    // Foam is a lit surface (responds to the sun, dims at night), kept softer so
    // it reads as spray rather than a bright decal.
    vec3 foamColor = uAmbient * 1.1 + uLightColor * max(dot(N, L), 0.0) * 0.8;
    color = mix(color, foamColor, foam * 0.7);

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

    FragColor = vec4(toOutput(color), 1.0);
}
