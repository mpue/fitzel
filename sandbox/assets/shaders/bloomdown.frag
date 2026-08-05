#version 330 core

// Bloom, step 1: downsample the previous level by 2.
//
// The 13-tap pattern (Jimenez / "next generation post processing", used by most
// engines) is what makes a bloom pyramid stable: a plain box or a sparse tap grid
// under-samples small bright features, so they beat against the pixel grid and
// crawl as moire when the camera moves -- exactly what emissive materials used to
// do here. On the first level the taps also run through the bright-pass knee and
// a Karis average, which weights each 2x2 group by 1/(1+luma): a single very
// bright texel then contributes its share instead of dominating the whole group
// and flickering (fireflies).

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uSrc;
uniform vec2  uSrcTexel;  // 1 / source resolution
uniform int   uFirstPass; // 1 = threshold + Karis (reading the scene HDR)
uniform float uThreshold; // luminance where bloom starts
uniform float uKnee;      // soft-knee width below the threshold

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Soft-knee bright pass: a hard cut-off pops as a surface crosses the threshold,
// the knee eases it in over `uKnee` of luminance.
vec3 prefilter(vec3 c) {
    float l = luma(c);
    float k = max(uKnee, 1.0e-4);
    float soft = clamp(l - uThreshold + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k);
    float w = max(soft, l - uThreshold) / max(l, 1.0e-4);
    return c * w;
}

vec3 tap(vec2 uv) {
    vec3 c = texture(uSrc, uv).rgb;
    return (uFirstPass == 1) ? prefilter(c) : c;
}

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec2 t  = uSrcTexel;

    vec3 a = tap(uv + vec2(-2.0,  2.0) * t);
    vec3 b = tap(uv + vec2( 0.0,  2.0) * t);
    vec3 c = tap(uv + vec2( 2.0,  2.0) * t);
    vec3 d = tap(uv + vec2(-2.0,  0.0) * t);
    vec3 e = tap(uv);
    vec3 f = tap(uv + vec2( 2.0,  0.0) * t);
    vec3 g = tap(uv + vec2(-2.0, -2.0) * t);
    vec3 h = tap(uv + vec2( 0.0, -2.0) * t);
    vec3 i = tap(uv + vec2( 2.0, -2.0) * t);
    vec3 j = tap(uv + vec2(-1.0,  1.0) * t);
    vec3 k = tap(uv + vec2( 1.0,  1.0) * t);
    vec3 l = tap(uv + vec2(-1.0, -1.0) * t);
    vec3 m = tap(uv + vec2( 1.0, -1.0) * t);

    // Five overlapping 2x2 boxes: the inner one carries half the weight.
    vec3 g0 = (a + b + d + e) * 0.25;
    vec3 g1 = (b + c + e + f) * 0.25;
    vec3 g2 = (d + e + g + h) * 0.25;
    vec3 g3 = (e + f + h + i) * 0.25;
    vec3 g4 = (j + k + l + m) * 0.25;

    vec3 sum;
    if (uFirstPass == 1) {
        // Karis average: weight each group by its own inverse luminance so one
        // blown-out texel can't make the whole level pulse frame to frame.
        float w0 = 0.125 / (1.0 + luma(g0));
        float w1 = 0.125 / (1.0 + luma(g1));
        float w2 = 0.125 / (1.0 + luma(g2));
        float w3 = 0.125 / (1.0 + luma(g3));
        float w4 = 0.500 / (1.0 + luma(g4));
        sum = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) /
              max(w0 + w1 + w2 + w3 + w4, 1.0e-4);
    } else {
        sum = g4 * 0.5 + (g0 + g1 + g2 + g3) * 0.125;
    }

    FragColor = vec4(sum, 1.0);
}
