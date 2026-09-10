#version 330 core

// The exposure meter: one pixel, the frame's centre-weighted mean log2
// luminance, eased towards over time.
//
// A single fragment reads a 32x18 grid of the HDR buffer. That is 576 taps for
// the whole frame -- nothing -- and far fewer would do: the answer is averaged
// in the log domain and then smoothed over half a second, so a sparse grid
// only has to be representative, not exact. The log average is the point: a
// frame that is a tenth sky must not be judged by the sky alone, which an
// arithmetic mean of radiance would do (the sun is thousands of times brighter
// than the ground).
//
// Output: R = the adapted value (what the composite exposes by), G = this
// frame's raw measurement (for the editor's readout).

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uHdr;
uniform sampler2D uPrev;   // last frame's 1x1 result
uniform float uBlend;      // 0..1 of the way to the measurement; 1 = snap

void main() {
    const int GX = 32, GY = 18;
    float sumL = 0.0, sumW = 0.0;
    for (int y = 0; y < GY; ++y) {
        for (int x = 0; x < GX; ++x) {
            vec2  uv = (vec2(x, y) + 0.5) / vec2(GX, GY);
            vec3  c  = textureLod(uHdr, uv, 0.0).rgb;
            float l  = dot(c, vec3(0.2126, 0.7152, 0.0722));
            if (isnan(l) || isinf(l)) continue;
            // Centre-weighted: what is in the middle of the frame is what is
            // being looked at, and a bright sky strip along the top is not.
            vec2  d = (uv - 0.5) * vec2(1.6, 1.0);
            float w = exp(-dot(d, d) * 3.0);
            sumL += log2(clamp(l, 1e-4, 6.0e4)) * w;
            sumW += w;
        }
    }
    float measured = sumL / max(sumW, 1e-6);
    float prev     = texelFetch(uPrev, ivec2(0), 0).r;
    if (isnan(prev) || isinf(prev)) prev = measured;
    FragColor = vec4(mix(prev, measured, clamp(uBlend, 0.0, 1.0)), measured, 0.0, 1.0);
}
