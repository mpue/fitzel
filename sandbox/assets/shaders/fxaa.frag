#version 330 core

// Classic FXAA (Timothy Lottes) over the final LDR image. Cheap edge-smoothing
// filter: detects luma edges and blends along them.
in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uImage;
uniform vec2  uTexel;   // 1 / resolution
uniform int   uEnabled;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec3 rgbM = texture(uImage, uv).rgb;
    if (uEnabled == 0) { FragColor = vec4(rgbM, 1.0); return; }

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
