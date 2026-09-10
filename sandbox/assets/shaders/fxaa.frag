#version 330 core

// Classic FXAA (Timothy Lottes) over the final LDR image. Cheap edge-smoothing
// filter: detects luma edges and blends along them.
in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uImage;
uniform vec2  uTexel;   // 1 / resolution
uniform int   uEnabled;
uniform float uSharpen;  // 0 = off; used when FXAA is off (TAA is on instead)

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Contrast-adaptive sharpening (AMD's CAS, the five-tap form). Temporal AA
// averages every pixel with its past and comes out a touch soft; an unsharp mask
// would buy the edge back and the aliasing with it. CAS scales its kernel by how
// much headroom the neighbourhood has, so it lifts texture and detail and leaves
// already-hard edges alone.
vec3 sharpen(vec2 uv, vec3 c) {
    vec3 n = texture(uImage, uv + vec2(0.0, -1.0) * uTexel).rgb;
    vec3 s = texture(uImage, uv + vec2(0.0,  1.0) * uTexel).rgb;
    vec3 e = texture(uImage, uv + vec2( 1.0, 0.0) * uTexel).rgb;
    vec3 w = texture(uImage, uv + vec2(-1.0, 0.0) * uTexel).rgb;
    vec3 mn = min(c, min(min(n, s), min(e, w)));
    vec3 mx = max(c, max(max(n, s), max(e, w)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 wgt = amp * (-1.0 / mix(8.0, 5.0, clamp(uSharpen, 0.0, 1.0)));
    return clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);
}

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec3 rgbM = texture(uImage, uv).rgb;
    if (uEnabled == 0) {
        FragColor = vec4(uSharpen > 0.001 ? sharpen(uv, rgbM) : rgbM, 1.0);
        return;
    }

    vec3 rgbNW = texture(uImage, uv + vec2(-1.0, -1.0) * uTexel).rgb;
    vec3 rgbNE = texture(uImage, uv + vec2( 1.0, -1.0) * uTexel).rgb;
    vec3 rgbSW = texture(uImage, uv + vec2(-1.0,  1.0) * uTexel).rgb;
    vec3 rgbSE = texture(uImage, uv + vec2( 1.0,  1.0) * uTexel).rgb;

    float lM = luma(rgbM);
    float lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // Skip flat areas (nothing to anti-alias).
    if (lMax - lMin < max(0.04, lMax * 0.125)) { FragColor = vec4(rgbM, 1.0); return; }

    vec2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcp, -8.0, 8.0) * uTexel;

    vec3 rgbA = 0.5 * (texture(uImage, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(uImage, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(uImage, uv + dir * -0.5).rgb +
                                     texture(uImage, uv + dir *  0.5).rgb);
    float lB = luma(rgbB);
    FragColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
